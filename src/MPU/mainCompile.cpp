#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>

#include <NvInfer.h>
#include <NvInferPlugin.h>
#include <cuda_runtime_api.h>
#include <cuda_fp16.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <curl/curl.h>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <thread>
#include <iomanip>
#include <sstream>
#include <deque>
#include <ctime>
#include <cstdio>

// ===== CAMERA STREAM TO WINFORMS (localhost TCP) =====
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <malloc.h>
#include <sys/syscall.h>
#include <cerrno>

#include "aiCore.hpp"

// Camera + dual TensorRT + server sync run continuously on independent workers.
using Clock = std::chrono::steady_clock;

static volatile std::sig_atomic_t g_stopSignal = 0;
void onSignal(int) { g_stopSignal = 1; }

static long systemProcessRssMB() {
    std::ifstream input("/proc/self/status");
    std::string key;

    while (input >> key) {
        if (key == "VmRSS:") {
            long valueKb = 0;
            std::string unit;
            input >> valueKb >> unit;
            return valueKb / 1024;
        }

        std::string rest;
        std::getline(input, rest);
    }

    return -1;
}

struct CameraFrame {
    cv::Mat image;  // Never modify this image; camera worker publishes a new buffer.
    uint64_t sequence = 0;
    Clock::time_point timestamp{};
};
struct CameraState {
    std::mutex mutex;
    std::condition_variable changed;
    CameraFrame newest;
    bool opened = false;
    bool finished = false;
    std::string error;
};

struct OverlayBox {
    cv::Rect rect;
    cv::Scalar color;
    std::string label;
};

struct VerifiedDefectRegion {
    // Normalized to the full decal box: x/y/w/h in approximately [0,1].
    cv::Rect2f norm;
    float confidence = 0.0f;
};

struct DefectDecalState {
    struct ConsensusRegion {
        cv::Rect2f norm;
        unsigned int history = 0;   // Sliding bitmask of REAL observations.
        float confidenceEma = 0.0f;
        uint64_t lastSeenFrame = 0;
    };

    size_t trackId = 0;

    // Filled by line crossing. 0 = not counted yet.
    size_t decalPosition = 0;

    // Becomes true only after at least one spatial consensus region passes.
    bool verified = false;
    bool saved = false;

    // Spatial consensus is kept per decal track, in normalized decal coordinates.
    std::vector<ConsensusRegion> consensusRegions;

    // Final evidence is ONE snapshot only.
    cv::Mat bestImage;

    // Boxes belong to the SAME frame as bestImage.
    // They contain only consensus-approved physical defects.
    std::vector<cv::Rect2f> bestBoxesNorm;

    float bestScore = -1.0f;
    double bestSharpness = 0.0;
    float bestAverageConfidence = 0.0f;

    uint64_t lastSeenFrame = 0;
    uint64_t lastVerifiedFrame = 0;
    uint64_t readySinceFrame = 0;

    // Filled once the final JPG is written locally.
    std::string savedFilePath;
    std::string savedFileName;
    std::string savedDateKey;
    std::string savedUniqueId;
    size_t savedErrorCount = 0;
    uint64_t savedTimestampMs = 0;

    // Background server sync state.
    // Once queued, the worker owns a snapshot of bestImage + metadata.
    bool uploadQueued = false;
    bool uploaded = false;
    std::string uploadedImageUrl;
};

static double rectIoU(const cv::Rect2f& a, const cv::Rect2f& b) {
    const cv::Rect2f inter = a & b;
    const double unionArea =
        static_cast<double>(a.area()) +
        static_cast<double>(b.area()) -
        static_cast<double>(inter.area());

    return unionArea > 0.0
        ? static_cast<double>(inter.area()) / unionArea
        : 0.0;
}

static double rectCenterDistance(const cv::Rect2f& a, const cv::Rect2f& b) {
    const cv::Point2f ca(
        a.x + a.width * 0.5f,
        a.y + a.height * 0.5f
    );

    const cv::Point2f cb(
        b.x + b.width * 0.5f,
        b.y + b.height * 0.5f
    );

    return cv::norm(ca - cb);
}

static double rectSizeRatio(const cv::Rect2f& a, const cv::Rect2f& b) {
    if (a.width <= 0 || a.height <= 0 ||
        b.width <= 0 || b.height <= 0) {
        return 1e9;
    }

    return std::max({
        static_cast<double>(a.width / b.width),
        static_cast<double>(b.width / a.width),
        static_cast<double>(a.height / b.height),
        static_cast<double>(b.height / a.height)
    });
}

static bool samePhysicalDefect(
        const cv::Rect2f& a,
        const cv::Rect2f& b) {

    const double iou = rectIoU(a, b);
    const double distance = rectCenterDistance(a, b);
    const double sizeRatio = rectSizeRatio(a, b);

    // Main rule: overlapping boxes are the same physical defect.
    if (iou >= 0.30)
        return true;

    // Fallback for a jittery detector whose box shape changes a lot:
    // centers within 2.5% of decal size + reasonably similar dimensions.
    if (distance <= 0.025 && sizeRatio <= 2.5)
        return true;

    return false;
}

static cv::Rect2f normalizeBoxToDecal(
        const cv::Rect& box,
        const cv::Rect& decalBox) {

    if (box.empty() || decalBox.empty())
        return {};

    const cv::Rect clipped = box & decalBox;

    if (clipped.empty())
        return {};

    return cv::Rect2f(
        static_cast<float>(clipped.x - decalBox.x) /
            static_cast<float>(decalBox.width),
        static_cast<float>(clipped.y - decalBox.y) /
            static_cast<float>(decalBox.height),
        static_cast<float>(clipped.width) /
            static_cast<float>(decalBox.width),
        static_cast<float>(clipped.height) /
            static_cast<float>(decalBox.height)
    );
}

static std::vector<TemporalErrorTracker::Confirmed>
deduplicateConfirmedErrors(
        const std::vector<TemporalErrorTracker::Confirmed>& input,
        const cv::Rect& decalBox) {

    std::vector<TemporalErrorTracker::Confirmed> output;
    std::vector<cv::Rect2f> normalized;

    if (decalBox.empty())
        return input;

    for (const auto& candidate : input) {
        const cv::Rect2f candidateNorm =
            normalizeBoxToDecal(candidate.box, decalBox);

        if (candidateNorm.area() <= 0)
            continue;

        int duplicateIndex = -1;
        double bestIoU = -1.0;

        for (size_t i = 0; i < normalized.size(); ++i) {
            if (!samePhysicalDefect(normalized[i], candidateNorm))
                continue;

            const double iou =
                rectIoU(normalized[i], candidateNorm);

            if (iou > bestIoU) {
                bestIoU = iou;
                duplicateIndex = static_cast<int>(i);
            }
        }

        if (duplicateIndex < 0) {
            output.push_back(candidate);
            normalized.push_back(candidateNorm);
            continue;
        }

        // Prefer a real observation over HOLD. If both are real/held, keep the
        // higher-confidence representative for the production overlay.
        auto& current =
            output[static_cast<size_t>(duplicateIndex)];

        const bool replace =
            (current.held && !candidate.held) ||
            (current.held == candidate.held &&
             candidate.confidence > current.confidence);

        if (replace) {
            current = candidate;
            normalized[static_cast<size_t>(duplicateIndex)] =
                candidateNorm;
        }
    }

    return output;
}


static int bitCount(unsigned int bits) {
    int count = 0;

    while (bits) {
        bits &= bits - 1;
        ++count;
    }

    return count;
}

static unsigned int historyMask(int window) {
    const int safeWindow = std::max(1, std::min(8, window));
    return (1u << safeWindow) - 1u;
}

static void updateDefectConsensus(
        const Config& c,
        DefectDecalState& state,
        const std::vector<TemporalErrorTracker::Confirmed>& confirmedErrors,
        const cv::Rect& decalBox,
        uint64_t currentFrame) {

    if (decalBox.empty())
        return;

    const unsigned int mask =
        historyMask(c.defectConsensusWindow);

    // Every AI frame advances the consensus window.
    for (auto& region : state.consensusRegions) {
        region.history =
            (region.history << 1) & mask;
    }

    std::vector<bool> regionUsed(
        state.consensusRegions.size(),
        false
    );

    for (const auto& error : confirmedErrors) {
        // HOLD is temporal persistence only, not a real observation.
        if (error.held)
            continue;

        const cv::Rect clipped =
            error.box & decalBox;

        if (clipped.empty())
            continue;

        const cv::Rect2f candidate =
            normalizeBoxToDecal(
                clipped,
                decalBox
            );

        if (candidate.area() <= 0)
            continue;

        int bestIndex = -1;
        double bestCost = 1e9;

        for (size_t i = 0;
             i < state.consensusRegions.size();
             ++i) {

            if (regionUsed[i])
                continue;

            const auto& region =
                state.consensusRegions[i];

            if (!samePhysicalDefect(
                    region.norm,
                    candidate
                ))
            {
                continue;
            }

            const double cost =
                (1.0 - rectIoU(
                    region.norm,
                    candidate
                )) +
                rectCenterDistance(
                    region.norm,
                    candidate
                ) * 3.0;

            if (cost < bestCost) {
                bestCost = cost;
                bestIndex =
                    static_cast<int>(i);
            }
        }

        if (bestIndex < 0) {
            DefectDecalState::ConsensusRegion region;
            region.norm = candidate;
            region.history = 1u;
            region.confidenceEma =
                error.confidence;
            region.lastSeenFrame =
                currentFrame;

            state.consensusRegions.push_back(
                region
            );

            regionUsed.push_back(true);
            continue;
        }

        auto& region =
            state.consensusRegions[
                static_cast<size_t>(bestIndex)
            ];

        // Smooth position jitter in normalized decal coordinates.
        const float alpha = 0.55f;

        region.norm.x =
            (1.0f - alpha) * region.norm.x +
            alpha * candidate.x;

        region.norm.y =
            (1.0f - alpha) * region.norm.y +
            alpha * candidate.y;

        region.norm.width =
            (1.0f - alpha) * region.norm.width +
            alpha * candidate.width;

        region.norm.height =
            (1.0f - alpha) * region.norm.height +
            alpha * candidate.height;

        region.confidenceEma =
            0.45f * region.confidenceEma +
            0.55f * error.confidence;

        region.history |= 1u;
        region.lastSeenFrame =
            currentFrame;

        regionUsed[
            static_cast<size_t>(bestIndex)
        ] = true;
    }

    // Remove regions that disappeared from the whole sliding window.
    state.consensusRegions.erase(
        std::remove_if(
            state.consensusRegions.begin(),
            state.consensusRegions.end(),
            [](const DefectDecalState::ConsensusRegion& region) {
                return region.history == 0;
            }
        ),
        state.consensusRegions.end()
    );

    // Production verification is LATCHED per decal track.
    // A defect that already passed temporal + spatial consensus must not
    // disappear again merely because later async frames miss it while the
    // label is moving out of the inspection zone.
    bool stableNow = false;

    for (const auto& region : state.consensusRegions) {
        if (bitCount(region.history) >=
                c.defectConsensusHits &&
            region.confidenceEma >=
                c.defectConsensusMinConfidence)
        {
            stableNow = true;
            break;
        }
    }

    if (stableNow) {
        const bool firstVerification =
            !state.verified;

        state.verified = true;
        state.lastVerifiedFrame =
            currentFrame;

        if (firstVerification) {
            std::cout
                << "[DEFECT] VERIFIED track="
                << state.trackId
                << " at aiFrame="
                << currentFrame
                << std::endl;
        }
    }
}

static bool consensusRegionStable(
        const Config& c,
        const DefectDecalState::ConsensusRegion& region) {

    return
        bitCount(region.history) >=
            c.defectConsensusHits &&
        region.confidenceEma >=
            c.defectConsensusMinConfidence;
}

static double laplacianSharpness(const cv::Mat& image) {
    if (image.empty()) return 0.0;

    cv::Mat gray;
    if (image.channels() == 3)
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    else
        gray = image;

    // Downscale only for scoring; the saved image remains full resolution.
    cv::Mat small = gray;
    if (gray.cols > 640) {
        const double scale = 640.0 / gray.cols;
        cv::resize(
            gray,
            small,
            cv::Size(
                640,
                std::max(1, static_cast<int>(std::round(gray.rows * scale)))
            ),
            0,
            0,
            cv::INTER_AREA
        );
    }

    cv::Mat lap;
    cv::Laplacian(small, lap, CV_64F);

    cv::Scalar mean, stddev;
    cv::meanStdDev(lap, mean, stddev);
    return stddev[0] * stddev[0];
}

static bool decalFullyVisible(const cv::Rect& box, const cv::Size& frameSize) {
    if (box.empty()) return false;

    // A small safety margin avoids saving a decal clipped by the camera edge.
    const int margin = 4;

    return box.x >= margin &&
           box.y >= margin &&
           box.br().x <= frameSize.width - margin &&
           box.br().y <= frameSize.height - margin;
}

static void updateBestVerifiedCapture(
        const Config& c,
        DefectDecalState& state,
        const cv::Mat& frame,
        const cv::Rect& decalBox,
        const std::vector<TemporalErrorTracker::Confirmed>& confirmedErrors) {

    if (frame.empty() || decalBox.empty())
        return;

    if (!decalFullyVisible(
            decalBox,
            frame.size()
        ))
    {
        return;
    }

    // Build REAL normalized boxes from this exact frame.
    struct CurrentBox {
        cv::Rect2f norm;
        float confidence = 0.0f;
    };

    std::vector<CurrentBox> currentBoxes;

    for (const auto& error : confirmedErrors) {
        if (error.held)
            continue;

        const cv::Rect clipped =
            error.box & decalBox;

        if (clipped.empty())
            continue;

        const cv::Rect2f norm =
            normalizeBoxToDecal(
                clipped,
                decalBox
            );

        if (norm.area() <= 0)
            continue;

        currentBoxes.push_back({
            norm,
            error.confidence
        });
    }

    if (currentBoxes.empty())
        return;

    // Only boxes matching a STABLE spatial-consensus region are allowed.
    std::vector<cv::Rect2f> approvedBoxes;
    std::vector<float> approvedConfidence;

    for (const auto& region : state.consensusRegions) {
        if (!consensusRegionStable(
                c,
                region
            ))
        {
            continue;
        }

        int bestIndex = -1;
        double bestCost = 1e9;

        for (size_t i = 0;
             i < currentBoxes.size();
             ++i) {

            if (!samePhysicalDefect(
                    region.norm,
                    currentBoxes[i].norm
                ))
            {
                continue;
            }

            const double cost =
                (1.0 - rectIoU(
                    region.norm,
                    currentBoxes[i].norm
                )) +
                rectCenterDistance(
                    region.norm,
                    currentBoxes[i].norm
                ) * 3.0;

            if (cost < bestCost) {
                bestCost = cost;
                bestIndex =
                    static_cast<int>(i);
            }
        }

        if (bestIndex < 0)
            continue;

        approvedBoxes.push_back(
            currentBoxes[
                static_cast<size_t>(bestIndex)
            ].norm
        );

        approvedConfidence.push_back(
            currentBoxes[
                static_cast<size_t>(bestIndex)
            ].confidence
        );
    }

    if (approvedBoxes.empty())
        return;

    // One last same-frame de-duplication after consensus matching.
    std::vector<cv::Rect2f> finalBoxes;
    std::vector<float> finalConfidence;

    for (size_t i = 0;
         i < approvedBoxes.size();
         ++i) {

        int duplicate = -1;

        for (size_t j = 0;
             j < finalBoxes.size();
             ++j) {

            if (samePhysicalDefect(
                    finalBoxes[j],
                    approvedBoxes[i]
                ))
            {
                duplicate =
                    static_cast<int>(j);
                break;
            }
        }

        if (duplicate < 0) {
            finalBoxes.push_back(
                approvedBoxes[i]
            );

            finalConfidence.push_back(
                approvedConfidence[i]
            );
        }
        else if (
            approvedConfidence[i] >
            finalConfidence[
                static_cast<size_t>(duplicate)
            ])
        {
            finalBoxes[
                static_cast<size_t>(duplicate)
            ] = approvedBoxes[i];

            finalConfidence[
                static_cast<size_t>(duplicate)
            ] = approvedConfidence[i];
        }
    }

    if (finalBoxes.empty())
        return;

    const cv::Mat cropView =
        frame(decalBox);

    const double sharpness =
        laplacianSharpness(cropView);

    float confidenceSum = 0.0f;

    for (float confidence : finalConfidence)
        confidenceSum += confidence;

    const float averageConfidence =
        confidenceSum /
        static_cast<float>(
            finalConfidence.size()
        );

    // Sharpness dominates. Box count is NOT rewarded.
    const float sharpnessScore =
        static_cast<float>(
            std::min(
                500.0,
                sharpness
            ) / 5.0
        );

    const float confidenceScore =
        averageConfidence * 12.0f;

    // Mild preference for fewer, cleaner regions if quality is otherwise equal.
    const float boxPenalty =
        finalBoxes.size() > 3
            ? static_cast<float>(
                  finalBoxes.size() - 3
              ) * 0.45f
            : 0.0f;

    const float score =
        sharpnessScore +
        confidenceScore -
        boxPenalty;

    if (score <= state.bestScore)
        return;

    state.bestImage =
        cropView.clone();

    state.bestBoxesNorm =
        std::move(finalBoxes);

    state.bestScore =
        score;

    state.bestSharpness =
        sharpness;

    state.bestAverageConfidence =
        averageConfidence;
}


struct VietnamStamp {
    std::string dateKey;   // YYYY-MM-DD
    std::string compact;   // YYYYMMDD_HHMMSS_mmm
    uint64_t epochMs = 0;
};

static VietnamStamp makeVietnamStamp() {
    using SysClock = std::chrono::system_clock;

    const auto now = SysClock::now();
    const auto epochMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();

    // Vietnam = UTC+7, no DST.
    const std::time_t vietnamSeconds =
        SysClock::to_time_t(now + std::chrono::hours(7));

    std::tm tm{};
    gmtime_r(&vietnamSeconds, &tm);

    const int millis =
        static_cast<int>(epochMs % 1000);

    std::ostringstream date;
    date << std::put_time(&tm, "%Y-%m-%d");

    std::ostringstream compact;
    compact
        << std::put_time(&tm, "%Y%m%d_%H%M%S")
        << "_"
        << std::setw(3)
        << std::setfill('0')
        << millis;

    return {
        date.str(),
        compact.str(),
        static_cast<uint64_t>(epochMs)
    };
}

static std::string makeVietnamDateKeyOffset(int dayOffset) {
    using SysClock = std::chrono::system_clock;

    const auto shifted =
        SysClock::now() +
        std::chrono::hours(7) +
        std::chrono::hours(24 * dayOffset);

    const std::time_t seconds =
        SysClock::to_time_t(shifted);

    std::tm tm{};
    gmtime_r(&seconds, &tm);

    std::ostringstream date;
    date << std::put_time(&tm, "%Y-%m-%d");
    return date.str();
}

static bool ensureDirectory(const std::string& path) {
    if (path.empty())
        return false;

    if (::mkdir(path.c_str(), 0755) == 0)
        return true;

    return errno == EEXIST;
}

static std::string jsonEscape(const std::string& value) {
    std::ostringstream out;

    for (unsigned char ch : value) {
        switch (ch) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b";  break;
            case '\f': out << "\\f";  break;
            case '\n': out << "\\n";  break;
            case '\r': out << "\\r";  break;
            case '\t': out << "\\t";  break;

            default:
                if (ch < 0x20) {
                    out
                        << "\\u"
                        << std::hex
                        << std::setw(4)
                        << std::setfill('0')
                        << static_cast<int>(ch)
                        << std::dec;
                }
                else {
                    out << static_cast<char>(ch);
                }
        }
    }

    return out.str();
}

static std::string urlEncode(CURL* curl, const std::string& value) {
    if (!curl)
        return {};

    char* encoded =
        curl_easy_escape(
            curl,
            value.c_str(),
            static_cast<int>(value.size())
        );

    if (!encoded)
        return {};

    std::string result(encoded);
    curl_free(encoded);
    return result;
}

static std::string base64Encode(const std::vector<unsigned char>& data) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;

    while (i + 2 < data.size()) {
        const unsigned int value =
            (static_cast<unsigned int>(data[i]) << 16) |
            (static_cast<unsigned int>(data[i + 1]) << 8) |
            static_cast<unsigned int>(data[i + 2]);

        out.push_back(table[(value >> 18) & 0x3F]);
        out.push_back(table[(value >> 12) & 0x3F]);
        out.push_back(table[(value >> 6) & 0x3F]);
        out.push_back(table[value & 0x3F]);

        i += 3;
    }

    if (i < data.size()) {
        unsigned int value =
            static_cast<unsigned int>(data[i]) << 16;

        out.push_back(table[(value >> 18) & 0x3F]);

        if (i + 1 < data.size()) {
            value |=
                static_cast<unsigned int>(data[i + 1]) << 8;

            out.push_back(table[(value >> 12) & 0x3F]);
            out.push_back(table[(value >> 6) & 0x3F]);
            out.push_back('=');
        }
        else {
            out.push_back(table[(value >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        }
    }

    return out;
}

static bool readBinaryFile(
        const std::string& path,
        std::vector<unsigned char>& bytes) {

    std::ifstream input(
        path,
        std::ios::binary | std::ios::ate
    );

    if (!input)
        return false;

    const std::streamsize size =
        input.tellg();

    if (size <= 0)
        return false;

    bytes.resize(static_cast<size_t>(size));
    input.seekg(0, std::ios::beg);

    return static_cast<bool>(
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            size
        )
    );
}

static std::string readTrimmedTextFile(const std::string& path) {
    std::ifstream input(path);

    if (!input)
        return {};

    std::string value(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>()
    );

    while (!value.empty() &&
           (value.back() == '\n' ||
            value.back() == '\r' ||
            value.back() == ' ' ||
            value.back() == '\t')) {
        value.pop_back();
    }

    size_t first = 0;

    while (first < value.size() &&
           (value[first] == ' ' ||
            value[first] == '\t' ||
            value[first] == '\n' ||
            value[first] == '\r')) {
        ++first;
    }

    return value.substr(first);
}

struct ThreadCurlHandle {
    CURL* value = nullptr;

    ThreadCurlHandle()
        : value(curl_easy_init()) {
    }

    ~ThreadCurlHandle() {
        if (value)
            curl_easy_cleanup(value);
    }
};

static CURL* reusableThreadCurl() {
    thread_local ThreadCurlHandle holder;

    if (!holder.value)
        return nullptr;

    // curl_easy_reset keeps the easy handle's connection/DNS/session caches,
    // allowing HTTP/TLS reuse while clearing request-specific options.
    curl_easy_reset(holder.value);
    return holder.value;
}

static size_t curlWriteToString(
        void* contents,
        size_t size,
        size_t nmemb,
        void* userp) {

    const size_t total = size * nmemb;

    if (userp && contents) {
        static_cast<std::string*>(userp)->append(
            static_cast<const char*>(contents),
            total
        );
    }

    return total;
}

struct DefectUploadJob {
    std::string localPath;
    std::string fileName;
    std::string dateKey;      // Image/GitHub folder date.
    std::string uniqueId;

    size_t position = 0;
    size_t trackId = 0;
    size_t errorCount = 0;

    uint64_t timestampMs = 0;

    // Capture production context before async upload starts.
    std::string productionDateKey;
    std::string productionRollId;
    size_t productionRollNumber = 0;
};

static bool saveVerifiedDefectCapture(
        const Config& c,
        DefectDecalState& state) {

    if (!state.verified ||
        state.decalPosition == 0 ||
        state.saved ||
        state.bestImage.empty() ||
        state.bestBoxesNorm.empty()) {
        return false;
    }

    try {
        if (!ensureDirectory(c.defectCaptureDir)) {
            throw std::runtime_error(
                "Cannot create capture directory: " +
                c.defectCaptureDir
            );
        }

        const VietnamStamp stamp =
            makeVietnamStamp();

        const std::string dayDirectory =
            c.defectCaptureDir +
            "/" +
            stamp.dateKey;

        if (!ensureDirectory(dayDirectory)) {
            throw std::runtime_error(
                "Cannot create daily capture directory: " +
                dayDirectory
            );
        }

        cv::Mat output = state.bestImage.clone();

        size_t drawIndex = 0;

        for (const auto& n : state.bestBoxesNorm) {

            cv::Rect box(
                cvRound(n.x * output.cols),
                cvRound(n.y * output.rows),
                std::max(1, cvRound(n.width * output.cols)),
                std::max(1, cvRound(n.height * output.rows))
            );

            box &= cv::Rect(0, 0, output.cols, output.rows);
            if (box.empty()) continue;

            ++drawIndex;

            cv::rectangle(
                output,
                box,
                cv::Scalar(0, 0, 255),
                2
            );

            cv::putText(
                output,
                "ERR " + std::to_string(drawIndex),
                cv::Point(
                    box.x,
                    std::max(20, box.y - 8)
                ),
                cv::FONT_HERSHEY_SIMPLEX,
                0.55,
                cv::Scalar(0, 0, 255),
                2
            );
        }

        // Count only VERIFIED boxes visible on the SAME selected snapshot.
        // Historical boxes from other moving frames are intentionally ignored.
        const size_t errorCount = state.bestBoxesNorm.size();

        cv::putText(
            output,
            "Decal #" + std::to_string(state.decalPosition) +
                " | Errors: " + std::to_string(errorCount),
            cv::Point(12, 28),
            cv::FONT_HERSHEY_SIMPLEX,
            0.65,
            cv::Scalar(0, 0, 255),
            2
        );

        // Unique even if position/track reset on another run.
        // Example:
        // decal_000007_track_8_errors_2_20261107_182501_123_pid10699.jpg
        std::ostringstream fileName;
        fileName
            << "decal_"
            << std::setw(6)
            << std::setfill('0')
            << state.decalPosition
            << "_track_"
            << state.trackId
            << "_errors_"
            << errorCount
            << "_"
            << stamp.compact
            << "_pid"
            << static_cast<long>(::getpid())
            << ".jpg";

        const std::string fullPath =
            dayDirectory +
            "/" +
            fileName.str();

        std::ostringstream uniqueId;
        uniqueId
            << "Defect_"
            << stamp.compact
            << "_pos"
            << std::setw(6)
            << std::setfill('0')
            << state.decalPosition
            << "_track"
            << state.trackId
            << "_pid"
            << static_cast<long>(::getpid());

        const std::vector<int> params = {
            cv::IMWRITE_JPEG_QUALITY,
            std::max(
                50,
                std::min(100, c.defectCaptureJpegQuality)
            )
        };

        if (!cv::imwrite(fullPath, output, params))
            return false;

        state.saved = true;
        state.savedFilePath = fullPath;
        state.savedFileName = fileName.str();
        state.savedDateKey = stamp.dateKey;
        state.savedUniqueId = uniqueId.str();
        state.savedErrorCount = errorCount;
        state.savedTimestampMs = stamp.epochMs;

        std::cout
            << "[DEFECT] Saved verified decal #"
            << state.decalPosition
            << " (track "
            << state.trackId
            << ", errors="
            << errorCount
            << ") -> "
            << fullPath
            << std::endl;

        return true;
    }
    catch (const std::exception& e) {
        std::cerr
            << "[DEFECT] Save failed for track "
            << state.trackId
            << ": "
            << e.what()
            << std::endl;

        return false;
    }
}




static void releaseDefectMemory(DefectDecalState& state) {
    state.bestImage.release();
    std::vector<cv::Rect2f>().swap(state.bestBoxesNorm);
    std::vector<DefectDecalState::ConsensusRegion>().swap(
        state.consensusRegions
    );
}

static void pruneDefectStates(
        std::unordered_map<size_t, DefectDecalState>& states,
        uint64_t currentFrame,
        int keepFrames,
        int orphanKeepFrames) {

    const uint64_t keep =
        static_cast<uint64_t>(std::max(60, keepFrames));

    const uint64_t orphanKeep =
        std::max<uint64_t>(
            static_cast<uint64_t>(
                std::max(120, orphanKeepFrames)
            ),
            keep
        );

    for (auto it = states.begin(); it != states.end();) {
        const uint64_t age =
            currentFrame >= it->second.lastSeenFrame
                ? currentFrame - it->second.lastSeenFrame
                : 0;

        uint64_t limit = keep;

        if (it->second.uploadQueued)
            limit = std::min<uint64_t>(keep, 60);
        else if (it->second.verified)
            limit = orphanKeep;

        if (it->second.lastSeenFrame != 0 && age > limit) {
            if (it->second.verified && !it->second.uploadQueued) {
                std::cerr
                    << "[MEM] Releasing stale VERIFIED orphan track="
                    << it->second.trackId
                    << " position="
                    << it->second.decalPosition
                    << " ageFrames="
                    << age
                    << std::endl;
            }

            releaseDefectMemory(it->second);
            it = states.erase(it);
        }
        else {
            ++it;
        }
    }
}



struct ProductionSnapshot {
    bool active = false;
    std::string dateKey;
    std::string rollId;
    size_t rollNumber = 0;
    size_t totalLabels = 0;
    size_t errorLabels = 0;
};

struct RollActionResult {
    bool ok = false;
    std::string dateKey;
    std::string rollId;
    size_t rollNumber = 0;
    size_t resumeTotal = 0;
    std::string message;
};

struct RollTotalJob {
    std::string dateKey;
    std::string rollId;
    size_t total = 0;

    bool valid() const {
        return !dateKey.empty() && !rollId.empty();
    }
};

static std::string trimText(std::string value) {
    auto isSpace = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };

    while (!value.empty() && isSpace(static_cast<unsigned char>(value.back())))
        value.pop_back();

    size_t first = 0;
    while (first < value.size() &&
           isSpace(static_cast<unsigned char>(value[first])))
        ++first;

    return value.substr(first);
}

static bool firebaseRestRequest(
        const Config& c,
        const std::string& method,
        const std::string& relativePath,
        const std::string& body,
        std::string* response = nullptr,
        long timeoutMs = 5000) {

    CURL* curl = reusableThreadCurl();
    if (!curl)
        return false;

    const std::string url =
        c.firebaseBaseUrl +
        "/" +
        relativePath +
        ".json";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string localResponse;

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 0L);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2500L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs);

    if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }
    else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(
            curl,
            CURLOPT_POSTFIELDSIZE,
            static_cast<long>(body.size())
        );
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &localResponse);

    const CURLcode result = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_slist_free_all(headers);

    if (response)
        *response = localResponse;

    const bool ok =
        result == CURLE_OK &&
        httpCode >= 200 &&
        httpCode < 300;

    if (!ok) {
        std::cerr
            << "[FIREBASE] "
            << method
            << " "
            << relativePath
            << " failed: "
            << curl_easy_strerror(result)
            << ", HTTP="
            << httpCode
            << std::endl;
    }

    return ok;
}

static bool firebaseGetRaw(
        const Config& c,
        const std::string& path,
        std::string& raw) {

    return firebaseRestRequest(
        c,
        "GET",
        path,
        "",
        &raw
    );
}

static bool firebasePutJson(
        const Config& c,
        const std::string& path,
        const std::string& json) {

    return firebaseRestRequest(
        c,
        "PUT",
        path,
        json,
        nullptr
    );
}

static bool parseJsonStringScalar(
        const std::string& rawValue,
        std::string& value) {

    const std::string raw =
        trimText(rawValue);

    if (raw.empty() || raw == "null")
        return false;

    if (raw.size() < 2 ||
        raw.front() != '"' ||
        raw.back() != '"')
        return false;

    value = raw.substr(1, raw.size() - 2);
    return true;
}

static size_t parseJsonSizeScalar(
        const std::string& rawValue,
        size_t fallback = 0) {

    const std::string raw =
        trimText(rawValue);

    if (raw.empty() || raw == "null")
        return fallback;

    try {
        return static_cast<size_t>(
            std::stoull(raw)
        );
    }
    catch (...) {
        return fallback;
    }
}

static size_t countTopLevelObjectMembers(
        const std::string& rawValue) {

    const std::string raw =
        trimText(rawValue);

    if (raw.empty() || raw == "null" || raw == "{}")
        return 0;

    int depth = 0;
    bool inString = false;
    bool escape = false;
    bool keyCandidate = false;
    size_t count = 0;

    for (size_t i = 0; i < raw.size(); ++i) {
        const char ch = raw[i];

        if (inString) {
            if (escape) {
                escape = false;
                continue;
            }

            if (ch == '\\') {
                escape = true;
                continue;
            }

            if (ch == '"') {
                inString = false;

                if (depth == 1 && keyCandidate) {
                    size_t j = i + 1;

                    while (j < raw.size() &&
                           (raw[j] == ' ' ||
                            raw[j] == '\t' ||
                            raw[j] == '\r' ||
                            raw[j] == '\n')) {
                        ++j;
                    }

                    if (j < raw.size() && raw[j] == ':')
                        ++count;
                }

                keyCandidate = false;
            }

            continue;
        }

        if (ch == '"') {
            inString = true;
            keyCandidate = depth == 1;
            continue;
        }

        if (ch == '{') {
            ++depth;
            continue;
        }

        if (ch == '}') {
            --depth;
            continue;
        }
    }

    return count;
}

class ProductionManager {
    const Config& config_;
    mutable std::mutex mutex_;

    std::string dateKey_;
    std::string currentRollId_;

    size_t rollCounter_ = 0;
    size_t currentRollNumber_ = 0;
    size_t currentTotal_ = 0;
    size_t currentErrorLabels_ = 0;

    bool initialized_ = false;

    std::string dayRoot(const std::string& dateKey) const {
        return config_.productionRoot + "/" + dateKey;
    }

    std::string runtimeRoot() const {
        return config_.productionRuntimeRoot;
    }

    bool getStringLocked(
            const std::string& path,
            std::string& value) const {

        std::string raw;

        if (!firebaseGetRaw(config_, path, raw))
            return false;

        if (!parseJsonStringScalar(raw, value))
            value.clear();

        return true;
    }

    bool getSizeLocked(
            const std::string& path,
            size_t& value) const {

        std::string raw;

        if (!firebaseGetRaw(config_, path, raw))
            return false;

        value = parseJsonSizeScalar(raw, 0);
        return true;
    }

    bool writeRuntimeLocked(uint64_t timestampMs) {
        bool ok = true;
        const std::string root = runtimeRoot();

        ok = firebasePutJson(
                config_, root + "/currentDateKey",
                dateKey_.empty()
                    ? "null"
                    : "\"" + jsonEscape(dateKey_) + "\""
            ) && ok;

        ok = firebasePutJson(
                config_, root + "/currentRollId",
                currentRollId_.empty()
                    ? "null"
                    : "\"" + jsonEscape(currentRollId_) + "\""
            ) && ok;

        ok = firebasePutJson(
                config_, root + "/currentRollNumber",
                std::to_string(currentRollNumber_)
            ) && ok;

        ok = firebasePutJson(
                config_, root + "/active",
                currentRollId_.empty() ? "false" : "true"
            ) && ok;

        ok = firebasePutJson(
                config_, root + "/updatedAt",
                std::to_string(timestampMs)
            ) && ok;

        return ok;
    }

    bool closeRollAtLocked(
            const std::string& dateKey,
            const std::string& rollId,
            size_t finalTotal,
            uint64_t endTimestampMs,
            const std::string& reason,
            bool clearDayCurrent) {

        if (dateKey.empty() || rollId.empty())
            return true;

        const std::string root = dayRoot(dateKey);
        bool ok = true;

        ok = firebasePutJson(
                config_,
                root + "/rolls/" + rollId + "/totalLabels",
                std::to_string(finalTotal)
            ) && ok;

        ok = firebasePutJson(
                config_,
                root + "/rolls/" + rollId + "/endTime",
                std::to_string(endTimestampMs)
            ) && ok;

        ok = firebasePutJson(
                config_,
                root + "/rolls/" + rollId + "/status",
                "\"closed\""
            ) && ok;

        ok = firebasePutJson(
                config_,
                root + "/rolls/" + rollId + "/closeReason",
                "\"" + jsonEscape(reason) + "\""
            ) && ok;

        if (clearDayCurrent) {
            ok = firebasePutJson(
                    config_, root + "/currentRollId", "null"
                ) && ok;

            ok = firebasePutJson(
                    config_, root + "/currentRollNumber", "0"
                ) && ok;

            ok = firebasePutJson(
                    config_, root + "/currentErrorLabels", "0"
                ) && ok;
        }

        return ok;
    }

    bool closeActiveRollLocked(
            size_t finalTotal,
            uint64_t endTimestampMs,
            const std::string& reason) {

        if (currentRollId_.empty())
            return true;

        const std::string closingDate = dateKey_;
        const std::string closingRoll = currentRollId_;
        const size_t closingErrors = currentErrorLabels_;

        const bool ok = closeRollAtLocked(
            closingDate,
            closingRoll,
            finalTotal,
            endTimestampMs,
            reason,
            true
        );

        if (!ok)
            return false;

        std::cout
            << "[PRODUCTION] Closed "
            << closingDate << " / " << closingRoll
            << " | total=" << finalTotal
            << " | defective labels=" << closingErrors
            << " | reason=" << reason
            << std::endl;

        currentRollId_.clear();
        currentRollNumber_ = 0;
        currentTotal_ = 0;
        currentErrorLabels_ = 0;

        (void)writeRuntimeLocked(endTimestampMs);
        return true;
    }

    bool loadDateLocked(const std::string& dateKey) {
        dateKey_ = dateKey;
        currentRollId_.clear();
        rollCounter_ = 0;
        currentRollNumber_ = 0;
        currentTotal_ = 0;
        currentErrorLabels_ = 0;

        const std::string root = dayRoot(dateKey_);

        getSizeLocked(root + "/rollCounter", rollCounter_);
        getStringLocked(root + "/currentRollId", currentRollId_);

        if (currentRollId_.empty()) {
            initialized_ = true;
            return true;
        }

        std::string status;
        getStringLocked(
            root + "/rolls/" + currentRollId_ + "/status",
            status
        );

        if (!status.empty() && status != "running") {
            currentRollId_.clear();
            initialized_ = true;
            return true;
        }

        getSizeLocked(
            root + "/rolls/" + currentRollId_ + "/rollNumber",
            currentRollNumber_
        );

        getSizeLocked(
            root + "/rolls/" + currentRollId_ + "/totalLabels",
            currentTotal_
        );

        getSizeLocked(
            root + "/rolls/" + currentRollId_ + "/errorLabels",
            currentErrorLabels_
        );

        firebasePutJson(
            config_, root + "/currentErrorLabels",
            std::to_string(currentErrorLabels_)
        );

        firebasePutJson(
            config_, root + "/currentRollNumber",
            std::to_string(currentRollNumber_)
        );

        initialized_ = true;

        std::cout
            << "[PRODUCTION] Resume "
            << dateKey_ << " / " << currentRollId_
            << " | roll=" << currentRollNumber_
            << " | total=" << currentTotal_
            << " | defective labels=" << currentErrorLabels_
            << std::endl;

        return true;
    }

    bool recoverStaleRuntimeLocked(const VietnamStamp& stamp) {
        std::string runtimeDate;
        std::string runtimeRoll;

        getStringLocked(
            runtimeRoot() + "/currentDateKey",
            runtimeDate
        );

        getStringLocked(
            runtimeRoot() + "/currentRollId",
            runtimeRoll
        );

        // Preferred path for all data created by this version onward.
        if (!runtimeDate.empty() &&
            !runtimeRoll.empty() &&
            runtimeDate != stamp.dateKey)
        {
            size_t total = 0;
            getSizeLocked(
                dayRoot(runtimeDate) +
                    "/rolls/" + runtimeRoll + "/totalLabels",
                total
            );

            std::cerr
                << "[PRODUCTION] Recover stale active roll "
                << runtimeDate << " / " << runtimeRoll
                << std::endl;

            const bool ok = closeRollAtLocked(
                runtimeDate,
                runtimeRoll,
                total,
                stamp.epochMs,
                "SYSTEM_RESTART_RECOVERY",
                true
            );

            if (!ok)
                return false;

            dateKey_ = stamp.dateKey;
            currentRollId_.clear();
            currentRollNumber_ = 0;
            return writeRuntimeLocked(stamp.epochMs);
        }

        // One-time compatibility repair for the old schema that had no
        // ProductionRuntime marker. It fixes the common case where yesterday
        // was left as RUNNING after the machine was powered off.
        if (runtimeRoll.empty()) {
            const std::string previousDate =
                makeVietnamDateKeyOffset(-1);

            std::string oldRoll;
            getStringLocked(
                dayRoot(previousDate) + "/currentRollId",
                oldRoll
            );

            if (!oldRoll.empty()) {
                std::string status;
                getStringLocked(
                    dayRoot(previousDate) +
                        "/rolls/" + oldRoll + "/status",
                    status
                );

                if (status.empty() || status == "running") {
                    size_t total = 0;
                    getSizeLocked(
                        dayRoot(previousDate) +
                            "/rolls/" + oldRoll + "/totalLabels",
                        total
                    );

                    std::cerr
                        << "[PRODUCTION] Repair legacy stale roll "
                        << previousDate << " / " << oldRoll
                        << std::endl;

                    if (!closeRollAtLocked(
                            previousDate,
                            oldRoll,
                            total,
                            stamp.epochMs,
                            "SYSTEM_RESTART_RECOVERY",
                            true
                        ))
                    {
                        return false;
                    }
                }
            }
        }

        return true;
    }

    bool createNewRollLocked(
            uint64_t startTimestampMs,
            RollActionResult& result,
            const std::string& continuedFrom = "") {

        const std::string root = dayRoot(dateKey_);
        ++rollCounter_;

        std::ostringstream rollId;
        rollId
            << "roll_"
            << std::setw(3)
            << std::setfill('0')
            << rollCounter_;

        const std::string newRollId = rollId.str();

        std::string body =
            "{"
            "\"rollNumber\":" + std::to_string(rollCounter_) + ","
            "\"totalLabels\":0,"
            "\"errorLabels\":0,"
            "\"startTime\":" + std::to_string(startTimestampMs) + ","
            "\"status\":\"running\"";

        if (!continuedFrom.empty()) {
            body +=
                ",\"continued\":true"
                ",\"continuedFrom\":\"" +
                jsonEscape(continuedFrom) + "\"";
        }

        body += "}";

        bool ok = firebasePutJson(
            config_,
            root + "/rolls/" + newRollId,
            body
        );

        ok = firebasePutJson(
                config_, root + "/rollCounter",
                std::to_string(rollCounter_)
            ) && ok;

        ok = firebasePutJson(
                config_, root + "/currentRollId",
                "\"" + jsonEscape(newRollId) + "\""
            ) && ok;

        ok = firebasePutJson(
                config_, root + "/currentRollNumber",
                std::to_string(rollCounter_)
            ) && ok;

        ok = firebasePutJson(
                config_, root + "/currentErrorLabels", "0"
            ) && ok;

        if (!ok) {
            result.ok = false;
            result.message = "ERR CREATE_ROLL";
            return false;
        }

        currentRollId_ = newRollId;
        currentRollNumber_ = rollCounter_;
        currentTotal_ = 0;
        currentErrorLabels_ = 0;

        (void)writeRuntimeLocked(startTimestampMs);

        result.ok = true;
        result.dateKey = dateKey_;
        result.rollId = currentRollId_;
        result.rollNumber = currentRollNumber_;
        result.resumeTotal = 0;

        std::ostringstream message;
        message
            << "OK NEW_ROLL "
            << currentRollNumber_
            << " "
            << currentRollId_;

        result.message = message.str();

        std::cout
            << "[PRODUCTION] Created "
            << currentRollId_
            << " on " << dateKey_;

        if (!continuedFrom.empty())
            std::cout << " | continuedFrom=" << continuedFrom;

        std::cout << std::endl;
        return true;
    }

public:
    explicit ProductionManager(const Config& config)
        : config_(config) {
    }

    bool initialize() {
        std::lock_guard<std::mutex> lock(mutex_);
        const VietnamStamp stamp = makeVietnamStamp();

        if (!recoverStaleRuntimeLocked(stamp))
            return false;

        if (!loadDateLocked(stamp.dateKey))
            return false;

        // Keep the runtime marker self-healing even if an older version left
        // an inconsistent same-day marker behind.
        (void)writeRuntimeLocked(stamp.epochMs);

        return true;
    }

    ProductionSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);

        ProductionSnapshot result;
        result.active = !currentRollId_.empty();
        result.dateKey = dateKey_;
        result.rollId = currentRollId_;
        result.rollNumber = currentRollNumber_;
        result.totalLabels = currentTotal_;
        result.errorLabels = currentErrorLabels_;
        return result;
    }

    bool needsDateRollover() const {
        const VietnamStamp stamp = makeVietnamStamp();
        std::lock_guard<std::mutex> lock(mutex_);
        return initialized_ &&
               !dateKey_.empty() &&
               stamp.dateKey != dateKey_;
    }

    bool startOrResumeSession(RollActionResult& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        const VietnamStamp stamp = makeVietnamStamp();

        if (!initialized_ || stamp.dateKey != dateKey_) {
            if (!loadDateLocked(stamp.dateKey)) {
                result.ok = false;
                result.message = "ERR LOAD_DATE";
                return false;
            }
        }

        if (!currentRollId_.empty()) {
            result.ok = true;
            result.dateKey = dateKey_;
            result.rollId = currentRollId_;
            result.rollNumber = currentRollNumber_;
            result.resumeTotal = currentTotal_;

            std::ostringstream message;
            message
                << "OK SESSION_RESUME "
                << currentRollNumber_
                << " "
                << currentRollId_;
            result.message = message.str();

            (void)writeRuntimeLocked(stamp.epochMs);
            return true;
        }

        return createNewRollLocked(
            stamp.epochMs,
            result
        );
    }

    bool closeSession(
            size_t finalTotal,
            RollActionResult& result,
            const std::string& reason) {

        std::lock_guard<std::mutex> lock(mutex_);
        const VietnamStamp stamp = makeVietnamStamp();

        if (currentRollId_.empty()) {
            result.ok = true;
            result.dateKey = dateKey_;
            result.message = "OK SESSION_CLOSED NO_ACTIVE_ROLL";
            return true;
        }

        const size_t closedRoll = currentRollNumber_;
        const std::string closedId = currentRollId_;

        if (!closeActiveRollLocked(
                finalTotal,
                stamp.epochMs,
                reason
            ))
        {
            result.ok = false;
            result.message = "ERR CLOSE_SESSION";
            return false;
        }

        result.ok = true;
        result.dateKey = dateKey_;
        result.rollId = closedId;
        result.rollNumber = closedRoll;
        result.resumeTotal = 0;

        std::ostringstream message;
        message
            << "OK SESSION_CLOSED "
            << closedRoll << " " << closedId;
        result.message = message.str();
        return true;
    }

    bool rolloverDayIfNeeded(
            size_t finalTotal,
            RollActionResult& result) {

        std::lock_guard<std::mutex> lock(mutex_);
        const VietnamStamp stamp = makeVietnamStamp();

        if (!initialized_) {
            loadDateLocked(stamp.dateKey);
            result.ok = true;
            result.resumeTotal = currentTotal_;
            result.dateKey = dateKey_;
            result.rollId = currentRollId_;
            result.rollNumber = currentRollNumber_;
            result.message = "OK DATE_INIT";
            return true;
        }

        if (stamp.dateKey == dateKey_)
            return false;

        const bool hadActive = !currentRollId_.empty();
        const std::string oldDate = dateKey_;
        const std::string oldRoll = currentRollId_;

        if (hadActive &&
            !closeActiveRollLocked(
                finalTotal,
                stamp.epochMs,
                "DAY_ROLLOVER"
            ))
        {
            result.ok = false;
            result.message = "ERR DATE_CLOSE";
            return false;
        }

        if (!loadDateLocked(stamp.dateKey)) {
            result.ok = false;
            result.message = "ERR DATE_LOAD";
            return false;
        }

        if (hadActive && currentRollId_.empty()) {
            return createNewRollLocked(
                stamp.epochMs,
                result,
                oldDate + "/" + oldRoll
            );
        }

        result.ok = true;
        result.dateKey = dateKey_;
        result.rollId = currentRollId_;
        result.rollNumber = currentRollNumber_;
        result.resumeTotal = currentTotal_;
        result.message = "OK DATE_ROLLOVER";
        return true;
    }

    bool startNewRoll(
            size_t finalTotal,
            RollActionResult& result) {

        std::lock_guard<std::mutex> lock(mutex_);
        const VietnamStamp stamp = makeVietnamStamp();

        if (!initialized_) {
            if (!loadDateLocked(stamp.dateKey)) {
                result.ok = false;
                result.message = "ERR LOAD_DATE";
                return false;
            }
        }

        if (stamp.dateKey != dateKey_) {
            if (!currentRollId_.empty() &&
                !closeActiveRollLocked(
                    finalTotal,
                    stamp.epochMs,
                    "DAY_ROLLOVER"
                ))
            {
                result.ok = false;
                result.message = "ERR CLOSE_ROLL";
                return false;
            }

            if (!loadDateLocked(stamp.dateKey)) {
                result.ok = false;
                result.message = "ERR LOAD_DATE";
                return false;
            }
        }

        if (!currentRollId_.empty() &&
            !closeActiveRollLocked(
                finalTotal,
                stamp.epochMs,
                "USER_ADD"
            ))
        {
            result.ok = false;
            result.message = "ERR CLOSE_ROLL";
            return false;
        }

        return createNewRollLocked(
            stamp.epochMs,
            result
        );
    }

    RollTotalJob makeTotalJob(size_t total) const {
        std::lock_guard<std::mutex> lock(mutex_);
        RollTotalJob job;

        if (currentRollId_.empty())
            return job;

        job.dateKey = dateKey_;
        job.rollId = currentRollId_;
        job.total = total;
        return job;
    }

    bool updateRollTotal(const RollTotalJob& job) {
        if (!job.valid())
            return true;

        const std::string root = dayRoot(job.dateKey);

        const bool ok = firebasePutJson(
            config_,
            root + "/rolls/" + job.rollId + "/totalLabels",
            std::to_string(job.total)
        );

        if (ok) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (job.dateKey == dateKey_ &&
                job.rollId == currentRollId_)
            {
                currentTotal_ = job.total;
            }
        }

        return ok;
    }

    bool recordDefect(
            const DefectUploadJob& job,
            const std::string& imageUrl) {

        if (job.productionDateKey.empty() ||
            job.productionRollId.empty())
        {
            std::cerr
                << "[FIREBASE] Defect has no production roll: "
                << job.uniqueId
                << std::endl;
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        const std::string root = dayRoot(job.productionDateKey);

        // INDUSTRIAL TRACEABILITY:
        // errorRolls is a subset/index of REAL production rolls. The node key
        // is the source roll itself (roll_001, roll_003, ...), never an
        // independent errorRoll_001 counter.
        const std::string errorRollId = job.productionRollId;
        const std::string errorRollPath =
            root + "/errorRolls/" + errorRollId;

        std::string existingRoll;
        if (!firebaseGetRaw(config_, errorRollPath, existingRoll))
            return false;

        if (trimText(existingRoll) == "null") {
            const std::string body =
                "{"
                "\"sourceRollId\":\"" +
                    jsonEscape(job.productionRollId) + "\","
                "\"sourceRollNumber\":" +
                    std::to_string(job.productionRollNumber) + ","
                "\"createdAt\":" +
                    std::to_string(job.timestampMs) +
                "}";

            if (!firebasePutJson(
                    config_,
                    errorRollPath,
                    body
                ))
            {
                return false;
            }
        }

        const std::string eventPath =
            errorRollPath + "/errors/" + job.uniqueId;

        std::string existing;
        if (!firebaseGetRaw(config_, eventPath, existing))
            return false;

        if (trimText(existing) == "null") {
            const std::string record =
                "{"
                "\"position\":" + std::to_string(job.position) + ","
                "\"errorCount\":" + std::to_string(job.errorCount) + ","
                "\"imageUrl\":\"" + jsonEscape(imageUrl) + "\","
                "\"timestamp\":" + std::to_string(job.timestampMs) +
                "}";

            if (!firebasePutJson(config_, eventPath, record))
                return false;
        }

        std::string errorsRaw;
        if (!firebaseGetRaw(
                config_,
                errorRollPath + "/errors",
                errorsRaw
            ))
        {
            return false;
        }

        const size_t defectiveLabelCount =
            countTopLevelObjectMembers(errorsRaw);

        bool ok = firebasePutJson(
            config_,
            root + "/rolls/" +
                job.productionRollId + "/errorLabels",
            std::to_string(defectiveLabelCount)
        );

        ok = firebasePutJson(
                config_,
                errorRollPath + "/defectiveLabels",
                std::to_string(defectiveLabelCount)
            ) && ok;

        if (job.productionDateKey == dateKey_ &&
            job.productionRollId == currentRollId_)
        {
            ok = firebasePutJson(
                    config_,
                    root + "/currentErrorLabels",
                    std::to_string(defectiveLabelCount)
                ) && ok;

            currentErrorLabels_ = defectiveLabelCount;
        }

        if (ok) {
            std::cout
                << "[FIREBASE] Defect saved: "
                << job.productionDateKey
                << " / " << job.productionRollId
                << " / " << job.uniqueId
                << " | position=" << job.position
                << " | errors=" << job.errorCount
                << " | defective labels=" << defectiveLabelCount
                << std::endl;
        }

        return ok;
    }
};

class ControlCommandChannel {
    mutable std::mutex mutex_;
    std::condition_variable changed_;

    uint64_t nextRequestId_ = 0;
    uint64_t completedRequestId_ = 0;

    bool pending_ = false;
    bool inFlight_ = false;

    std::string pendingCommand_;
    std::string response_;

public:
    bool request(
            const std::string& command,
            std::string& response,
            int timeoutMs) {

        std::unique_lock<std::mutex> lock(mutex_);

        if (pending_ || inFlight_) {
            response = "BUSY " + command;
            return false;
        }

        const uint64_t requestId =
            ++nextRequestId_;

        pending_ = true;
        pendingCommand_ = command;
        response_.clear();

        changed_.notify_all();

        const bool completed =
            changed_.wait_for(
                lock,
                std::chrono::milliseconds(timeoutMs),
                [&] {
                    return
                        completedRequestId_ >= requestId;
                }
            );

        if (!completed) {
            response = "QUEUED " + command;
            return true;
        }

        response = response_;
        return true;
    }

    bool take(
            uint64_t& requestId,
            std::string& command) {

        std::lock_guard<std::mutex> lock(mutex_);

        if (!pending_ || inFlight_)
            return false;

        pending_ = false;
        inFlight_ = true;

        requestId = nextRequestId_;
        command = pendingCommand_;

        return true;
    }

    void complete(
            uint64_t requestId,
            const std::string& response) {

        {
            std::lock_guard<std::mutex> lock(mutex_);

            completedRequestId_ =
                std::max(
                    completedRequestId_,
                    requestId
                );

            response_ = response;
            inFlight_ = false;
            pendingCommand_.clear();
        }

        changed_.notify_all();
    }
};


struct AnalysisState {
    std::mutex mutex;
    std::vector<OverlayBox> boxes;
    size_t total = 0;
    size_t errorsInFrame = 0;   // Confirmed errors, not single-frame guesses.
    size_t rawErrorsInFrame = 0;
    cv::Mat analyzedImage;     // Camera image corresponding to the bounding boxes.
    uint64_t analyzedSequence = 0;
    bool ready = false;
    bool failed = false;
    std::string error;
    Clock::time_point frameTimestamp{};
};

// The network may take seconds; never invoke HTTP from the inference thread.
// Pending updates are coalesced (latest cumulative total wins) and retried.
// Firebase total writes are intentionally synchronous only at explicit
// STOP / QUIT / ADD synchronization boundaries.
static bool githubUploadImage(
        const Config& c,
        const DefectUploadJob& job,
        std::string& imageUrl) {

    if (!c.githubUploadEnabled)
        return false;

    const std::string token =
        readTrimmedTextFile(c.githubTokenFile);

    if (token.empty()) {
        std::cerr
            << "[GITHUB] Token missing/empty: "
            << c.githubTokenFile
            << std::endl;
        return false;
    }

    std::vector<unsigned char> bytes;

    if (!readBinaryFile(job.localPath, bytes)) {
        std::cerr
            << "[GITHUB] Cannot read image: "
            << job.localPath
            << std::endl;
        return false;
    }

    const std::string contentBase64 =
        base64Encode(bytes);

    // Different folder every day + different filename every event.
    const std::string remotePath =
        c.githubImageRoot +
        "/" +
        job.dateKey +
        "/" +
        job.fileName;

    CURL* curl = reusableThreadCurl();

    if (!curl)
        return false;

    const std::string encodedPath =
        urlEncode(curl, remotePath);

    std::string apiPath = encodedPath;

    // Preserve '/' path separators after curl_easy_escape.
    for (size_t pos = 0;
         (pos = apiPath.find("%2F", pos)) != std::string::npos;) {
        apiPath.replace(pos, 3, "/");
        ++pos;
    }

    for (size_t pos = 0;
         (pos = apiPath.find("%2f", pos)) != std::string::npos;) {
        apiPath.replace(pos, 3, "/");
        ++pos;
    }

    const std::string url =
        "https://api.github.com/repos/" +
        c.githubOwner +
        "/" +
        c.githubRepo +
        "/contents/" +
        apiPath;

    const std::string body =
        "{"
        "\"message\":\"" +
            jsonEscape(
                "Add verified defect " +
                job.uniqueId
            ) +
        "\","
        "\"content\":\"" +
            contentBase64 +
        "\","
        "\"branch\":\"" +
            jsonEscape(c.githubBranch) +
        "\""
        "}";

    struct curl_slist* headers = nullptr;

    headers = curl_slist_append(
        headers,
        "Accept: application/vnd.github+json"
    );

    const std::string auth =
        "Authorization: Bearer " + token;

    headers = curl_slist_append(
        headers,
        auth.c_str()
    );

    headers = curl_slist_append(
        headers,
        "X-GitHub-Api-Version: 2026-03-10"
    );

    headers = curl_slist_append(
        headers,
        "User-Agent: DAKTNC-Jetson-Nano"
    );

    headers = curl_slist_append(
        headers,
        "Content-Type: application/json"
    );

    std::string response;

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 0L);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(
        curl,
        CURLOPT_POSTFIELDSIZE,
        static_cast<long>(body.size())
    );
    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        curlWriteToString
    );
    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response
    );
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 20000L);

    const CURLcode result =
        curl_easy_perform(curl);

    long httpCode = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &httpCode
    );

    curl_slist_free_all(headers);

    const bool success =
        result == CURLE_OK &&
        httpCode >= 200 &&
        httpCode < 300;

    if (!success) {
        std::cerr
            << "[GITHUB] Upload failed: "
            << curl_easy_strerror(result)
            << ", HTTP="
            << httpCode
            << std::endl;

        if (!response.empty()) {
            std::cerr
                << "[GITHUB] Response: "
                << response.substr(0, 500)
                << std::endl;
        }

        return false;
    }

    // Immediate public URL; no need to wait for GitHub Pages deploy.
    imageUrl =
        "https://raw.githubusercontent.com/" +
        c.githubOwner +
        "/" +
        c.githubRepo +
        "/" +
        c.githubBranch +
        "/" +
        remotePath;

    std::cout
        << "[GITHUB] Uploaded: "
        << remotePath
        << std::endl;

    return true;
}

// ============================================================
// BACKGROUND SERVER SYNC
//
// TRUE PARALLEL NETWORK PIPELINE:
//   Worker A: Firebase live total updates
//   Worker B: JPEG + GitHub + Firebase defect metadata
//
// Neither worker executes on an AI thread, and GitHub latency cannot delay
// the live total displayed by the web dashboard.
// ============================================================
class BackgroundSyncWorker {
    struct CaptureEnvelope {
        DefectDecalState state;
        ProductionSnapshot production;
        int attempts = 0;
    };

    struct NetworkEnvelope {
        DefectUploadJob job;
        std::string imageUrl;
        bool githubUploaded = false;
        int attempts = 0;
    };

    const Config& config_;
    ProductionManager& production_;

    // Only this queue may retain full cv::Mat evidence.
    std::mutex captureMutex_;
    std::condition_variable captureChanged_;
    std::thread captureWorker_;
    std::deque<CaptureEnvelope> captures_;
    bool captureBusy_ = false;
    bool captureQueueFullAnnounced_ = false;

    // Disk-backed network queue: filepath + metadata only, NO cv::Mat.
    std::mutex networkMutex_;
    std::condition_variable networkChanged_;
    std::thread networkWorker_;
    std::deque<NetworkEnvelope> network_;
    bool networkBusy_ = false;

    // Independent realtime total updater.
    std::mutex totalMutex_;
    std::condition_variable totalChanged_;
    std::thread totalWorker_;
    RollTotalJob latestTotal_;
    bool hasTotal_ = false;
    bool totalBusy_ = false;

    std::atomic<bool> stopping_{false};

    void captureRun() {
        setCurrentThreadNice(config_.uploadWorkerNice);

        while (true) {
            CaptureEnvelope capture;

            {
                std::unique_lock<std::mutex> lock(captureMutex_);
                captureChanged_.wait(lock, [&] {
                    return stopping_.load() || !captures_.empty();
                });

                if (stopping_.load() && captures_.empty())
                    return;

                capture = std::move(captures_.front());
                captures_.pop_front();
                captureBusy_ = true;
            }

            bool encoded = capture.state.saved;

            if (!encoded) {
                encoded = saveVerifiedDefectCapture(
                    config_,
                    capture.state
                );
            }

            if (encoded && capture.state.saved) {
                NetworkEnvelope job;

                job.job.localPath = capture.state.savedFilePath;
                job.job.fileName = capture.state.savedFileName;
                job.job.dateKey = capture.production.dateKey;
                job.job.uniqueId = capture.state.savedUniqueId;
                job.job.position = capture.state.decalPosition;
                job.job.trackId = capture.state.trackId;
                job.job.errorCount = capture.state.savedErrorCount;
                job.job.timestampMs = capture.state.savedTimestampMs;
                job.job.productionDateKey = capture.production.dateKey;
                job.job.productionRollId = capture.production.rollId;
                job.job.productionRollNumber = capture.production.rollNumber;

                {
                    std::lock_guard<std::mutex> lock(networkMutex_);

                    if (network_.size() < config_.maxNetworkQueue) {
                        network_.push_back(std::move(job));
                    }
                    else {
                        std::cerr
                            << "[UPLOAD] Disk/network queue full; JPG remains local: "
                            << capture.state.savedFilePath
                            << std::endl;
                    }
                }

                networkChanged_.notify_one();

                std::cout
                    << "[DEFECT] DISK_READY "
                    << capture.state.savedFileName
                    << " -> network queue"
                    << std::endl;

                capture.state.bestImage.release();
                capture.state.bestBoxesNorm.clear();
                capture.state.consensusRegions.clear();

                ::malloc_trim(0);
            }
            else {
                ++capture.attempts;

                if (capture.attempts < 3 && !stopping_.load()) {
                    {
                        std::lock_guard<std::mutex> lock(captureMutex_);
                        captures_.push_back(std::move(capture));
                    }
                    captureChanged_.notify_one();
                }
                else {
                    std::cerr
                        << "[DEFECT] JPEG encode failed after retries."
                        << std::endl;
                }
            }

            {
                std::lock_guard<std::mutex> lock(captureMutex_);
                captureBusy_ = false;
            }
        }
    }

    void networkRun() {
        setCurrentThreadNice(config_.uploadWorkerNice);

        while (true) {
            NetworkEnvelope envelope;

            {
                std::unique_lock<std::mutex> lock(networkMutex_);
                networkChanged_.wait(lock, [&] {
                    return stopping_.load() || !network_.empty();
                });

                if (stopping_.load() && network_.empty())
                    return;

                envelope = std::move(network_.front());
                network_.pop_front();
                networkBusy_ = true;
            }

            // Base64 + JSON only needs a small transient buffer now that raw
            // images are disk-backed. Pause only in a TRUE emergency (<64 MB).
            bool lowMemoryLogged = false;

            while (!stopping_.load()) {
                const long availableMb =
                    systemMemAvailableMB();

                if (availableMb < 0 ||
                    availableMb >=
                        config_.minFreeMemoryForUploadMB)
                {
                    break;
                }

                if (!lowMemoryLogged) {
                    std::cerr
                        << "[UPLOAD] PAUSED low memory: "
                        << availableMb
                        << " MB available"
                        << std::endl;

                    lowMemoryLogged = true;
                }

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(500)
                );
            }

            const long uploadMemMb =
                systemMemAvailableMB();

            std::cout
                << "[UPLOAD] START "
                << envelope.job.fileName
                << " | MemAvailable="
                << uploadMemMb
                << " MB"
                << std::endl;

            bool success = false;

            if (!envelope.githubUploaded) {
                std::string imageUrl;

                if (githubUploadImage(
                        config_,
                        envelope.job,
                        imageUrl
                    ))
                {
                    envelope.imageUrl = imageUrl;
                    envelope.githubUploaded = true;
                }
            }

            if (envelope.githubUploaded) {
                success = production_.recordDefect(
                    envelope.job,
                    envelope.imageUrl
                );
            }

            if (!success) {
                ++envelope.attempts;

                if (envelope.attempts < 6 && !stopping_.load()) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(
                            500 * envelope.attempts
                        )
                    );

                    {
                        std::lock_guard<std::mutex> lock(networkMutex_);
                        network_.push_back(std::move(envelope));
                    }

                    networkChanged_.notify_one();
                }
                else {
                    std::cerr
                        << "[UPLOAD] Network sync failed; JPG remains local: "
                        << envelope.job.localPath
                        << std::endl;
                }
            }

            ::malloc_trim(0);

            {
                std::lock_guard<std::mutex> lock(networkMutex_);
                networkBusy_ = false;
            }
        }
    }

    void totalRun() {
        setCurrentThreadNice(config_.totalWorkerNice);

        while (true) {
            RollTotalJob total;

            {
                std::unique_lock<std::mutex> lock(totalMutex_);
                totalChanged_.wait(lock, [&] {
                    return stopping_.load() || hasTotal_;
                });

                if (stopping_.load() && !hasTotal_)
                    return;

                total = latestTotal_;
                hasTotal_ = false;
                totalBusy_ = true;
            }

            const bool ok = production_.updateRollTotal(total);

            if (ok) {
                std::cout
                    << "[WEB] Total synced: "
                    << total.total
                    << " -> "
                    << total.rollId
                    << std::endl;
            }
            else if (!stopping_.load()) {
                {
                    std::lock_guard<std::mutex> lock(totalMutex_);

                    if (!hasTotal_ ||
                        latestTotal_.total < total.total)
                    {
                        latestTotal_ = total;
                        hasTotal_ = true;
                    }
                }

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(250)
                );
                totalChanged_.notify_one();
            }

            {
                std::lock_guard<std::mutex> lock(totalMutex_);
                totalBusy_ = false;
            }
        }
    }

    bool allDefectsIdle() {
        bool captureIdle;
        bool networkIdle;

        {
            std::lock_guard<std::mutex> lock(captureMutex_);
            captureIdle = captures_.empty() && !captureBusy_;
        }

        {
            std::lock_guard<std::mutex> lock(networkMutex_);
            networkIdle = network_.empty() && !networkBusy_;
        }

        return captureIdle && networkIdle;
    }

    bool totalIdle() {
        std::lock_guard<std::mutex> lock(totalMutex_);
        return !hasTotal_ && !totalBusy_;
    }

public:
    BackgroundSyncWorker(
            const Config& config,
            ProductionManager& production)
        : config_(config),
          production_(production)
    {
        captureWorker_ = std::thread(
            &BackgroundSyncWorker::captureRun,
            this
        );

        networkWorker_ = std::thread(
            &BackgroundSyncWorker::networkRun,
            this
        );

        totalWorker_ = std::thread(
            &BackgroundSyncWorker::totalRun,
            this
        );
    }

    ~BackgroundSyncWorker() {
        stopping_.store(true);

        captureChanged_.notify_all();
        networkChanged_.notify_all();
        totalChanged_.notify_all();

        if (captureWorker_.joinable())
            captureWorker_.join();

        if (networkWorker_.joinable())
            networkWorker_.join();

        if (totalWorker_.joinable())
            totalWorker_.join();
    }

    bool publishDefect(
            const DefectDecalState& state,
            const ProductionSnapshot& production) {

        if (!production.active ||
            !state.verified ||
            state.decalPosition == 0 ||
            state.bestImage.empty() ||
            state.bestBoxesNorm.empty())
        {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(captureMutex_);

            if (captures_.size() >= config_.maxUploadQueue) {
                if (!captureQueueFullAnnounced_) {
                    captureQueueFullAnnounced_ = true;

                    std::cerr
                        << "[UPLOAD] RAW image queue full; "
                        << "AI retains current defect and retries."
                        << std::endl;
                }

                return false;
            }

            captureQueueFullAnnounced_ = false;

            CaptureEnvelope envelope;
            envelope.state = state;
            envelope.production = production;

            captures_.push_back(std::move(envelope));
        }

        captureChanged_.notify_one();
        return true;
    }

    // ADD is a production boundary. It must NEVER wait for GitHub/Firebase,
    // but it also must not lose verified defects that could not enter the tiny
    // RAW queue. Persist those remaining defects to disk synchronously, then
    // hand only filepath + metadata to the network worker.
    bool persistDefectForRollBoundary(
            DefectDecalState& state,
            const ProductionSnapshot& production) {

        if (!production.active ||
            state.uploadQueued ||
            !state.verified ||
            state.decalPosition == 0 ||
            state.bestImage.empty() ||
            state.bestBoxesNorm.empty())
        {
            return false;
        }

        // Do not encode another JPG if the metadata queue is already full.
        // Otherwise repeated retries could create duplicate files on disk.
        {
            std::lock_guard<std::mutex> lock(networkMutex_);
            if (network_.size() >= config_.maxNetworkQueue) {
                std::cerr
                    << "[UPLOAD] Network metadata queue full; defer track "
                    << state.trackId
                    << std::endl;
                return false;
            }
        }

        DefectDecalState persisted = state;

        if (!persisted.saved) {
            if (!saveVerifiedDefectCapture(
                    config_,
                    persisted
                ))
            {
                return false;
            }
        }

        NetworkEnvelope networkJob;

        networkJob.job.localPath =
            persisted.savedFilePath;

        networkJob.job.fileName =
            persisted.savedFileName;

        networkJob.job.dateKey =
            production.dateKey;

        networkJob.job.uniqueId =
            persisted.savedUniqueId;

        networkJob.job.position =
            persisted.decalPosition;

        networkJob.job.trackId =
            persisted.trackId;

        networkJob.job.errorCount =
            persisted.savedErrorCount;

        networkJob.job.timestampMs =
            persisted.savedTimestampMs;

        networkJob.job.productionDateKey =
            production.dateKey;

        networkJob.job.productionRollId =
            production.rollId;

        networkJob.job.productionRollNumber =
            production.rollNumber;

        {
            std::lock_guard<std::mutex> lock(
                networkMutex_
            );

            // This queue holds metadata only. 64 jobs is already tiny in RAM.
            // If it is ever full, keep the JPG safely on disk and report failure
            // so ADD will not silently discard the defect.
            if (network_.size() >=
                config_.maxNetworkQueue)
            {
                std::cerr
                    << "[ADD] Network metadata queue full; cannot safely "
                    << "finalize defect track "
                    << state.trackId
                    << std::endl;

                return false;
            }

            network_.push_back(
                std::move(networkJob)
            );
        }

        networkChanged_.notify_one();

        // Transfer complete: the evidence is durable on disk and its metadata
        // is owned by the network queue. Release inference-side heavy buffers.
        state.uploadQueued = true;
        releaseDefectMemory(state);

        ::malloc_trim(0);

        return true;
    }

    void publishTotal(const RollTotalJob& job) {
        if (!job.valid())
            return;

        {
            std::lock_guard<std::mutex> lock(totalMutex_);

            if (!hasTotal_ ||
                latestTotal_.rollId != job.rollId ||
                latestTotal_.dateKey != job.dateKey ||
                job.total >= latestTotal_.total)
            {
                latestTotal_ = job;
                hasTotal_ = true;
            }
        }

        totalChanged_.notify_one();
    }

    bool waitIdle(int timeoutMs) {
        const auto deadline =
            Clock::now() +
            std::chrono::milliseconds(timeoutMs);

        while (Clock::now() < deadline) {
            if (allDefectsIdle() && totalIdle())
                return true;

            std::this_thread::sleep_for(
                std::chrono::milliseconds(20)
            );
        }

        return allDefectsIdle() && totalIdle();
    }

    size_t pendingDefects() {
        size_t raw = 0;
        size_t disk = 0;

        {
            std::lock_guard<std::mutex> lock(captureMutex_);
            raw = captures_.size() + (captureBusy_ ? 1u : 0u);
        }

        {
            std::lock_guard<std::mutex> lock(networkMutex_);
            disk = network_.size() + (networkBusy_ ? 1u : 0u);
        }

        return raw + disk;
    }
};



// ============================================================
// USB CAMERA CAPTURE
//
// Latest-frame only. Camera never waits for TensorRT/network workers.
// ============================================================
void captureLoop(const Config& c,
                 CameraState& state,
                 std::atomic<bool>& stop,
                 Clock::time_point startedAt) {
    (void)startedAt;

    cv::VideoCapture camera;

    try {
        // USB webcam only: V4L2.
        camera.open(
            c.cameraIndex,
            cv::CAP_V4L2
        );

        if (!camera.isOpened()) {
            throw std::runtime_error(
                "Cannot open USB camera /dev/video" +
                std::to_string(c.cameraIndex)
            );
        }

        // MJPG is normally much lighter on USB2 bandwidth than raw YUYV
        // at 1280x720 / 30 FPS.
        if (c.useMJPG) {
            camera.set(
                cv::CAP_PROP_FOURCC,
                cv::VideoWriter::fourcc(
                    'M',
                    'J',
                    'P',
                    'G'
                )
            );
        }

        camera.set(
            cv::CAP_PROP_FRAME_WIDTH,
            c.width
        );

        camera.set(
            cv::CAP_PROP_FRAME_HEIGHT,
            c.height
        );

        camera.set(
            cv::CAP_PROP_FPS,
            c.fps
        );

        // Driver may ignore this, but when supported it prevents an old-frame
        // queue from building inside V4L2/OpenCV.
        camera.set(
            cv::CAP_PROP_BUFFERSIZE,
            1
        );

        std::cout
            << "[USB] /dev/video"
            << c.cameraIndex
            << " | "
            << camera.get(
                   cv::CAP_PROP_FRAME_WIDTH
               )
            << "x"
            << camera.get(
                   cv::CAP_PROP_FRAME_HEIGHT
               )
            << " @ "
            << camera.get(
                   cv::CAP_PROP_FPS
               )
            << " FPS"
            << std::endl;

        {
            std::lock_guard<std::mutex> lock(
                state.mutex
            );

            state.opened = true;
        }

        state.changed.notify_all();

        cv::Mat image;
        int consecutiveErrors = 0;

        while (!stop.load()) {
            if (!camera.read(image) ||
                image.empty())
            {
                ++consecutiveErrors;

                if (consecutiveErrors >= 10) {
                    throw std::runtime_error(
                        "Camera stopped providing frames"
                    );
                }

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(20)
                );

                continue;
            }

            consecutiveErrors = 0;

            if (c.cameraFlip >= -1 &&
                c.cameraFlip <= 1)
            {
                cv::flip(
                    image,
                    image,
                    c.cameraFlip
                );
            }

            const auto now =
                Clock::now();

            {
                std::lock_guard<std::mutex> lock(
                    state.mutex
                );

                // Latest-frame buffer:
                // inference/preview own references to previous allocations,
                // while capture immediately replaces the newest frame.
                state.newest.image =
                    image.clone();

                state.newest.timestamp =
                    now;

                ++state.newest.sequence;

                if (state.newest.sequence == 1) {
                    std::cout
                        << "[STARTUP] Camera ready"
                        << std::endl;
                }
            }

            state.changed.notify_all();
        }
    }
    catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(
            state.mutex
        );

        state.error =
            e.what();
    }

    camera.release();

    {
        std::lock_guard<std::mutex> lock(
            state.mutex
        );

        state.finished = true;
    }

    state.changed.notify_all();
}


void inferenceLoop(const Config& c,
                   CameraState& cam,
                   AnalysisState& state,
                   ProductionManager& production,
                   BackgroundSyncWorker& syncWorker,
                   ControlCommandChannel& controlCommands,
                   std::atomic<bool>& stop,
                   Clock::time_point startedAt) {
    try {
        // HIGH-priority CUDA stream: counting/tracking is the real-time path.
        Detector decalDetector(
            c,
            c.decalModel,
            c.lowConfidence,
            c.iou,
            true
        );

        // LOW-priority CUDA stream + separate CPU worker: er1 never blocks
        // best.engine on the host side. Pending error jobs are latest-only.
        std::unique_ptr<AsyncErrorWorker> errorWorker;

        if (c.enableErrorModel)
            errorWorker.reset(new AsyncErrorWorker(c));

        const cv::Mat warmup(
            std::max(1,c.inputHeight),
            std::max(1,c.inputWidth),
            CV_8UC3,
            cv::Scalar(114,114,114)
        );

        (void)decalDetector.detect(warmup);

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.ready = true;
        }

        std::cout
            << "[STARTUP] Async dual TensorRT ready"
            << " | decal=HIGH CUDA priority"
            << " | error=LOW CUDA priority/latest-only"
            << " | "
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                   Clock::now()-startedAt
               ).count()
            << " ms"
            << std::endl;

        local_track::Tracker tracker(
            60,
            c.confidence,
            c.newConfidence
        );

        StableCounter counter(c.count);
        StableErrorROI errorROISelector;

        std::unordered_map<
            size_t,
            std::unique_ptr<TemporalErrorTracker>
        > errorTrackers;

        struct DisplayTrack {
            cv::Rect box;
            uint64_t lastFrame = 0;
        };

        std::unordered_map<size_t, DisplayTrack> displayTracks;
        std::unordered_map<size_t, DefectDecalState> defectStates;

        ProductionSnapshot productionState =
            production.snapshot();

        size_t rollBaseTotal =
            productionState.active
                ? productionState.totalLabels
                : 0;

        uint64_t errorEpoch = 1;
        uint64_t lastSequence = 0;
        uint64_t processedFrames = 0;
        double latestErrorMs = 0.0;
        Clock::time_point lastMemoryLog = Clock::now();

        auto resetRollRuntime = [&] {
            tracker.reset();
            counter.reset();
            errorROISelector.reset();

            std::unordered_map<
                size_t,
                std::unique_ptr<TemporalErrorTracker>
            >().swap(errorTrackers);

            std::unordered_map<size_t, DisplayTrack>().swap(
                displayTracks
            );

            for (auto& entry : defectStates)
                releaseDefectMemory(entry.second);

            std::unordered_map<size_t, DefectDecalState>().swap(
                defectStates
            );

            ++errorEpoch;

            if (errorWorker)
                errorWorker->clear();

            ::malloc_trim(0);
        };

        auto makeTotalJobCached = [&](size_t total) {
            RollTotalJob job;

            if (productionState.active) {
                job.dateKey = productionState.dateKey;
                job.rollId = productionState.rollId;
                job.total = total;
            }

            return job;
        };

        auto queueReadyDefects =
            [&](bool force) -> size_t {

                if (!productionState.active)
                    return 0;

                size_t queued = 0;

                for (auto& entry : defectStates) {
                    DefectDecalState& defectState = entry.second;

                    if (defectState.uploadQueued ||
                        !defectState.verified ||
                        defectState.decalPosition == 0 ||
                        defectState.bestImage.empty() ||
                        defectState.bestBoxesNorm.empty())
                    {
                        continue;
                    }

                    if (!force) {
                        if (defectState.readySinceFrame == 0) {
                            defectState.readySinceFrame = processedFrames;
                            continue;
                        }

                        const uint64_t delay =
                            static_cast<uint64_t>(
                                std::max(
                                    0,
                                    c.defectFinalizeDelayFrames
                                )
                            );

                        if (processedFrames -
                                defectState.readySinceFrame < delay)
                        {
                            continue;
                        }
                    }

                    if (syncWorker.publishDefect(
                            defectState,
                            productionState
                        ))
                    {
                        std::cout
                            << "[DEFECT] QUEUED track="
                            << defectState.trackId
                            << " position="
                            << defectState.decalPosition
                            << " rawQueue->JPEG"
                            << std::endl;

                        defectState.uploadQueued = true;

                        // BackgroundSyncWorker::publishDefect copied the state.
                        // cv::Mat is reference-counted, so the worker now owns
                        // the full-resolution evidence buffer. Release the
                        // inference-side reference immediately to keep RAM bounded.
                        releaseDefectMemory(defectState);

                        ++queued;
                    }
                }

                return queued;
            };

        auto persistBoundaryDefects =
            [&]() -> std::pair<size_t, size_t> {

                size_t persisted = 0;
                size_t failed = 0;

                if (!productionState.active)
                    return {persisted, failed};

                for (auto& entry : defectStates) {
                    DefectDecalState& defectState =
                        entry.second;

                    if (defectState.uploadQueued ||
                        !defectState.verified ||
                        defectState.decalPosition == 0 ||
                        defectState.bestImage.empty() ||
                        defectState.bestBoxesNorm.empty())
                    {
                        continue;
                    }

                    if (syncWorker.persistDefectForRollBoundary(
                            defectState,
                            productionState
                        ))
                    {
                        ++persisted;
                    }
                    else {
                        ++failed;
                    }
                }

                return {persisted, failed};
            };

        auto flushRollData =
            [&](const std::string& reason,
                bool waitForServer) -> std::string {

                if (!productionState.active)
                    return "OK FLUSH NO_ACTIVE_ROLL";

                const size_t finalTotal =
                    rollBaseTotal + counter.total();

                const size_t queued =
                    queueReadyDefects(true);

                syncWorker.publishTotal(
                    makeTotalJobCached(finalTotal)
                );

                bool idle = true;

                if (waitForServer) {
                    idle = syncWorker.waitIdle(120000);
                }

                std::ostringstream response;
                response
                    << (idle ? "OK FLUSH " : "WARN FLUSH ")
                    << "TOTAL=" << finalTotal
                    << " QUEUED=" << queued
                    << " SERVER=" << (idle ? "IDLE" : "BUSY");

                std::cout
                    << "[SYNC] "
                    << reason
                    << " -> "
                    << response.str()
                    << std::endl;

                return response.str();
            };

        while (!stop.load()) {
            CameraFrame snapshot;

            {
                std::unique_lock<std::mutex> lock(cam.mutex);

                cam.changed.wait_for(
                    lock,
                    std::chrono::milliseconds(100),
                    [&] {
                        return stop.load() ||
                               cam.newest.sequence > lastSequence ||
                               cam.finished;
                    }
                );

                if (stop.load())
                    break;

                if (cam.newest.sequence <= lastSequence) {
                    if (cam.finished)
                        break;

                    continue;
                }

                snapshot = cam.newest;
            }

            lastSequence = snapshot.sequence;
            ++processedFrames;

            // ------------------------------------------------------------
            // Control commands are rare roll-boundary operations.
            // Normal STOP no longer needs this path because server sync is
            // already live in BackgroundSyncWorker.
            // ------------------------------------------------------------
            uint64_t requestId = 0;
            std::string controlCommand;

            if (controlCommands.take(
                    requestId,
                    controlCommand
                ))
            {
                if (controlCommand == "APP_START") {
                    RollActionResult action;

                    const bool ok =
                        production.startOrResumeSession(action);

                    if (ok) {
                        resetRollRuntime();
                        productionState = production.snapshot();
                        rollBaseTotal = action.resumeTotal;
                    }

                    controlCommands.complete(
                        requestId,
                        action.message.empty()
                            ? (ok ? "OK APP_START" : "ERR APP_START")
                            : action.message
                    );
                }
                else if (controlCommand == "SESSION_CLOSE") {
                    const size_t finalTotal =
                        productionState.active
                            ? rollBaseTotal + counter.total()
                            : 0;

                    const size_t asyncQueued =
                        queueReadyDefects(true);

                    const auto boundaryPersist =
                        persistBoundaryDefects();

                    if (boundaryPersist.second != 0) {
                        std::ostringstream error;
                        error
                            << "ERR SESSION_CLOSE_PERSIST_FAILED "
                            << boundaryPersist.second;

                        controlCommands.complete(
                            requestId,
                            error.str()
                        );
                        continue;
                    }

                    syncWorker.publishTotal(
                        makeTotalJobCached(finalTotal)
                    );

                    RollActionResult action;
                    const bool ok = production.closeSession(
                        finalTotal,
                        action,
                        "APP_QUIT"
                    );

                    if (ok) {
                        resetRollRuntime();
                        productionState = production.snapshot();
                        rollBaseTotal = 0;

                        std::cout
                            << "[SESSION] Closed locally"
                            << " | total=" << finalTotal
                            << " | rawQueued=" << asyncQueued
                            << " | diskPersisted="
                            << boundaryPersist.first
                            << " | uploads=BACKGROUND"
                            << std::endl;
                    }

                    controlCommands.complete(
                        requestId,
                        action.message.empty()
                            ? (ok ? "OK SESSION_CLOSED" : "ERR SESSION_CLOSE")
                            : action.message
                    );
                }
                else if (controlCommand == "FLUSH" ||
                    controlCommand == "STOP")
                {
                    const std::string response =
                        flushRollData(
                            controlCommand,
                            true
                        );

                    controlCommands.complete(
                        requestId,
                        response
                    );
                }
                else if (controlCommand == "ADD") {
                    const size_t finalTotal =
                        productionState.active
                            ? rollBaseTotal + counter.total()
                            : 0;

                    // ----------------------------------------------------
                    // ADD = ROLL BOUNDARY, NOT "WAIT FOR INTERNET".
                    //
                    // 1) Put as many verified defects as possible into the
                    //    tiny asynchronous RAW queue.
                    // 2) Any verified defect that did not fit is encoded to
                    //    local JPG NOW and only lightweight metadata is queued.
                    // 3) Close old roll + create new roll immediately.
                    // GitHub/Firebase defect uploads continue in background
                    // using the OLD productionRollId captured in each job.
                    // ----------------------------------------------------
                    const size_t asyncQueued =
                        queueReadyDefects(true);

                    const auto boundaryPersist =
                        persistBoundaryDefects();

                    const size_t diskPersisted =
                        boundaryPersist.first;

                    const size_t persistFailed =
                        boundaryPersist.second;

                    // Final total update can remain asynchronous. startNewRoll()
                    // also writes finalTotal while closing the old roll.
                    syncWorker.publishTotal(
                        makeTotalJobCached(finalTotal)
                    );

                    if (persistFailed != 0) {
                        std::ostringstream error;

                        error
                            << "ERR ADD_PERSIST_FAILED "
                            << persistFailed;

                        controlCommands.complete(
                            requestId,
                            error.str()
                        );

                        std::cerr
                            << "[ADD] Aborted: "
                            << persistFailed
                            << " verified defect(s) could not be persisted."
                            << std::endl;

                        continue;
                    }

                    RollActionResult action;

                    const bool ok =
                        production.startNewRoll(
                            finalTotal,
                            action
                        );

                    if (ok) {
                        resetRollRuntime();

                        productionState =
                            production.snapshot();

                        rollBaseTotal =
                            action.resumeTotal;

                        controlCommands.complete(
                            requestId,
                            action.message
                        );

                        std::cout
                            << "[ADD] oldTotal="
                            << finalTotal
                            << " | rawQueued="
                            << asyncQueued
                            << " | diskPersisted="
                            << diskPersisted
                            << " | serverUpload=BACKGROUND"
                            << " -> "
                            << action.message
                            << std::endl;
                    }
                    else {
                        controlCommands.complete(
                            requestId,
                            action.message.empty()
                                ? "ERR NEW_ROLL"
                                : action.message
                        );
                    }
                }
                else {
                    controlCommands.complete(
                        requestId,
                        "ERR UNKNOWN_COMMAND"
                    );
                }
            }

            // Midnight is a FAILSAFE production boundary. Never wait for
            // GitHub/network here; verified evidence is first made durable
            // locally and retains the OLD date/roll metadata.
            if (processedFrames % 60 == 0) {
                const VietnamStamp nowStamp = makeVietnamStamp();

                if (!productionState.dateKey.empty() &&
                    nowStamp.dateKey != productionState.dateKey)
                {
                    const size_t finalTotal =
                        productionState.active
                            ? rollBaseTotal + counter.total()
                            : 0;

                    const size_t asyncQueued =
                        queueReadyDefects(true);

                    const auto boundaryPersist =
                        persistBoundaryDefects();

                    if (boundaryPersist.second == 0) {
                        syncWorker.publishTotal(
                            makeTotalJobCached(finalTotal)
                        );

                        RollActionResult action;

                        if (production.rolloverDayIfNeeded(
                                finalTotal,
                                action
                            ))
                        {
                            resetRollRuntime();
                            productionState = production.snapshot();
                            rollBaseTotal = action.resumeTotal;

                            std::cout
                                << "[DATE] Rollover"
                                << " | total=" << finalTotal
                                << " | rawQueued=" << asyncQueued
                                << " | diskPersisted="
                                << boundaryPersist.first
                                << " | " << action.message
                                << std::endl;
                        }
                    }
                    else {
                        std::cerr
                            << "[DATE] Rollover deferred: "
                            << boundaryPersist.second
                            << " defect(s) are not durable yet."
                            << std::endl;
                    }
                }
            }

            const cv::Mat& frame = snapshot.image;

            const int extent =
                c.axis == "x"
                    ? frame.cols
                    : frame.rows;

            if (c.line-c.count.margin < 0 ||
                c.line+c.count.margin >= extent)
            {
                throw std::runtime_error(
                    "Counting line and margin must fit inside image on selected axis"
                );
            }

            // ============================================================
            // HIGH-PRIORITY REAL-TIME PATH: best.engine
            // ============================================================
            const auto decalStarted = Clock::now();

            auto decalDetections =
                decalDetector.detect(frame);

            const double decalMs =
                std::chrono::duration<double, std::milli>(
                    Clock::now() - decalStarted
                ).count();

            auto tracks =
                tracker.update(decalDetections);

            const ErrorROISelection chosen =
                errorROISelector.select(
                    c,
                    frame.size(),
                    tracks
                );

            if (errorWorker &&
                chosen.fresh &&
                chosen.id != 0 &&
                !chosen.roi.empty())
            {
                errorWorker->publishLatest(
                    errorEpoch,
                    snapshot.sequence,
                    processedFrames,
                    chosen.id,
                    chosen.roi,
                    chosen.decalBox,
                    snapshot.image
                );
            }

            // ============================================================
            // Consume a COMPLETED low-priority error result if one exists.
            // Never wait for it.
            // ============================================================
            std::vector<local_track::Object> rawErrors;
            std::vector<TemporalErrorTracker::Confirmed> productionVerified;

            AsyncErrorWorker::Result errorResult;

            if (errorWorker &&
                errorWorker->tryTakeLatest(errorResult) &&
                errorResult.epoch == errorEpoch &&
                errorResult.decalId != 0)
            {
                rawErrors = std::move(errorResult.raw);
                latestErrorMs = errorResult.processingMs;

                auto& trackerPtr =
                    errorTrackers[errorResult.decalId];

                if (!trackerPtr) {
                    trackerPtr.reset(
                        new TemporalErrorTracker(
                            c.errorWindow,
                            c.errorConfirmHits,
                            c.errorMaxMissed,
                            c.errorConfirmConfidence,
                            c.errorWeakStableConfidence
                        )
                    );
                }

                const auto confirmedErrors =
                    trackerPtr->update(
                        rawErrors,
                        errorResult.roi,
                        errorResult.frame.size()
                    );

                const auto uniqueConfirmedErrors =
                    deduplicateConfirmedErrors(
                        confirmedErrors,
                        errorResult.decalBox
                    );

                auto& defectState =
                    defectStates[errorResult.decalId];

                defectState.trackId = errorResult.decalId;
                defectState.lastSeenFrame =
                    std::max(
                        defectState.lastSeenFrame,
                        errorResult.aiFrame
                    );

                updateDefectConsensus(
                    c,
                    defectState,
                    uniqueConfirmedErrors,
                    errorResult.decalBox,
                    errorResult.aiFrame
                );

                if (defectState.verified &&
                    !defectState.uploadQueued)
                {
                    // The uploader already owns the finalized evidence once queued.
                    // Do not recreate another full-resolution bestImage for the
                    // same decal track while it remains visible.
                    updateBestVerifiedCapture(
                        c,
                        defectState,
                        errorResult.frame,
                        errorResult.decalBox,
                        uniqueConfirmedErrors
                    );
                }

                for (const auto& error : uniqueConfirmedErrors) {
                    if (error.held)
                        continue;

                    const cv::Rect clipped =
                        error.box & errorResult.decalBox;

                    if (clipped.empty())
                        continue;

                    const cv::Rect2f norm =
                        normalizeBoxToDecal(
                            clipped,
                            errorResult.decalBox
                        );

                    bool stable = false;

                    for (const auto& region :
                         defectState.consensusRegions)
                    {
                        if (consensusRegionStable(c, region) &&
                            samePhysicalDefect(region.norm, norm))
                        {
                            stable = true;
                            break;
                        }
                    }

                    if (stable)
                        productionVerified.push_back(error);
                }

                productionVerified =
                    deduplicateConfirmedErrors(
                        productionVerified,
                        errorResult.decalBox
                    );
            }

            // ============================================================
            // Counting/tracking stays entirely local and never touches HTTP.
            // ============================================================
            for (const auto& track : tracks) {
                const auto& r = track->getRect();
                const size_t id = track->getTrackId();

                const int x1 = static_cast<int>(r.x());
                const int y1 = static_cast<int>(r.y());
                const int x2 = static_cast<int>(r.br_x());
                const int y2 = static_cast<int>(r.br_y());

                const double center =
                    c.axis == "x"
                        ? (r.x() + r.br_x()) * 0.5
                        : (r.y() + r.br_y()) * 0.5;

                auto& defectState = defectStates[id];
                defectState.trackId = id;
                defectState.lastSeenFrame = processedFrames;

                if (productionState.active &&
                    counter.observe(
                        id,
                        center,
                        processedFrames
                    ))
                {
                    const size_t position =
                        rollBaseTotal + counter.total();

                    defectState.decalPosition = position;

                    // REAL-TIME WEB UPDATE.
                    // This goes to a dedicated Firebase thread and can run
                    // concurrently with GitHub defect image uploads.
                    syncWorker.publishTotal(
                        makeTotalJobCached(position)
                    );
                }

                if (counter.confirmed(id)) {
                    displayTracks[id] = {
                        cv::Rect(
                            x1,
                            y1,
                            std::max(1, x2 - x1),
                            std::max(1, y2 - y1)
                        ),
                        processedFrames
                    };
                }
            }

            // Verified evidence moves to the background queue as soon as the
            // sharp-frame delay is satisfied. No JPEG/Base64/network here.
            (void)queueReadyDefects(false);

            std::vector<OverlayBox> boxes;

            for (auto it = displayTracks.begin();
                 it != displayTracks.end();)
            {
                const uint64_t missing =
                    processedFrames - it->second.lastFrame;

                if (missing >
                    static_cast<uint64_t>(c.holdFrames))
                {
                    it = displayTracks.erase(it);
                    continue;
                }

                const cv::Scalar color =
                    missing
                        ? cv::Scalar(150,150,150)
                        : (counter.counted(it->first)
                            ? cv::Scalar(255,200,0)
                            : cv::Scalar(0,255,0));

                boxes.push_back({
                    it->second.box,
                    color,
                    "ID " +
                        std::to_string(it->first) +
                        (missing ? " HOLD" : "")
                });

                ++it;
            }

            const size_t errorsInFrame =
                productionVerified.size();

            for (size_t i = 0;
                 i < productionVerified.size() &&
                 i < static_cast<size_t>(c.maxErrorBoxes);
                 ++i)
            {
                const auto& e = productionVerified[i];

                boxes.push_back({
                    e.box,
                    cv::Scalar(0,0,255),
                    "ERR " +
                        std::to_string(i + 1) +
                        " " +
                        std::to_string(
                            static_cast<int>(
                                std::round(
                                    e.confidence * 100
                                )
                            )
                        ) +
                        "%"
                });
            }

            counter.prune(processedFrames);

            // A VERIFIED state is never allowed to own a full-resolution
            // cv::Mat forever. If it has left the camera, persist it directly
            // to disk/network metadata before the normal prune pass.
            if (productionState.active) {
                const uint64_t staleThreshold =
                    static_cast<uint64_t>(
                        std::max(60, c.defectStateKeepFrames)
                    );

                for (auto& entry : defectStates) {
                    DefectDecalState& defectState = entry.second;

                    if (!defectState.verified ||
                        defectState.uploadQueued ||
                        defectState.lastSeenFrame == 0)
                    {
                        continue;
                    }

                    const uint64_t age =
                        processedFrames >= defectState.lastSeenFrame
                            ? processedFrames - defectState.lastSeenFrame
                            : 0;

                    if (age <= staleThreshold ||
                        defectState.decalPosition == 0 ||
                        defectState.bestImage.empty() ||
                        defectState.bestBoxesNorm.empty())
                    {
                        continue;
                    }

                    if (syncWorker.persistDefectForRollBoundary(
                            defectState,
                            productionState
                        ))
                    {
                        std::cout
                            << "[MEM] Persisted stale VERIFIED track="
                            << defectState.trackId
                            << " before prune"
                            << std::endl;
                    }
                }
            }

            pruneDefectStates(
                defectStates,
                processedFrames,
                c.defectStateKeepFrames,
                c.defectOrphanKeepFrames
            );

            for (auto it = errorTrackers.begin();
                 it != errorTrackers.end();)
            {
                if (defectStates.find(it->first) ==
                    defectStates.end())
                {
                    it = errorTrackers.erase(it);
                }
                else {
                    ++it;
                }
            }

            const auto memoryNow = Clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    memoryNow - lastMemoryLog
                ).count() >= c.memoryLogSeconds)
            {
                std::cout
                    << "[MEM] RSS=" << systemProcessRssMB() << " MB"
                    << " | Available=" << systemMemAvailableMB() << " MB"
                    << " | defectStates=" << defectStates.size()
                    << " | errorTrackers=" << errorTrackers.size()
                    << " | uploadJobs=" << syncWorker.pendingDefects()
                    << std::endl;

                lastMemoryLog = memoryNow;
            }

            if (c.perfLogEnabled &&
                processedFrames % 120 == 0)
            {
                std::cout
                    << "[PERF] decal="
                    << std::fixed
                    << std::setprecision(1)
                    << decalMs
                    << " ms | error_async="
                    << latestErrorMs
                    << " ms | uploadQ="
                    << syncWorker.pendingDefects()
                    << std::endl;
            }

            // OpenCV/libstdc++ may keep freed heap pages in glibc arenas.
            // Return free pages to Linux periodically to give Mono/Xorg/VNC
            // more headroom on the 4 GB Jetson Nano.
            if (processedFrames % 600 == 0) {
                ::malloc_trim(0);
            }

            {
                std::lock_guard<std::mutex> lock(state.mutex);

                state.boxes = std::move(boxes);
                state.total =
                    productionState.active
                        ? rollBaseTotal + counter.total()
                        : 0;

                state.errorsInFrame = errorsInFrame;
                state.rawErrorsInFrame = rawErrors.size();

                if (c.syncAIFrame) {
                    state.analyzedImage = snapshot.image;
                }
                else {
                    state.analyzedImage.release();
                }

                state.analyzedSequence = snapshot.sequence;
                state.frameTimestamp = snapshot.timestamp;
            }
        }
    }
    catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.failed = true;
        state.error = e.what();
    }
}


// ============================================================
// WINFORMS CAMERA STREAM SERVER
// Protocol: [4-byte JPEG length, network byte order] + [JPEG bytes]
// Only the newest frame is kept, so a slow UI cannot build a stale queue.
// Listen only on 127.0.0.1:5000 (local Jetson App).
// ============================================================
class JpegFrameServer {
    int serverFd_ = -1;
    int clientFd_ = -1;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable changed_;

    // cv::Mat is reference-counted. We keep only the newest preview frame.
    // No deep clone is needed because the caller never modifies display
    // after publish().
    cv::Mat latestFrame_;
    uint64_t version_ = 0;
    uint64_t sentVersion_ = 0;
    bool stopping_ = false;

    int previewWidth_;
    int jpegQuality_;

    static bool sendAll(int fd, const void* data, size_t size) {
        const char* p = static_cast<const char*>(data);

        while (size > 0) {
            const ssize_t n = ::send(fd, p, size, MSG_NOSIGNAL);

            if (n <= 0)
                return false;

            p += n;
            size -= static_cast<size_t>(n);
        }

        return true;
    }

    void closeClient() {
        if (clientFd_ >= 0) {
            ::close(clientFd_);
            clientFd_ = -1;
        }
    }

    void run() {
        setCurrentThreadNice(8);

        // Reuse JPEG memory instead of allocating a new vector every frame.
        std::vector<uchar> jpeg;
        jpeg.reserve(256 * 1024);

        const std::vector<int> encodeParams = {
            cv::IMWRITE_JPEG_QUALITY,
            jpegQuality_
        };

        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (stopping_)
                    break;
            }

            if (clientFd_ < 0) {
                sockaddr_in client{};
                socklen_t clientLen = sizeof(client);

                const int fd = ::accept(
                    serverFd_,
                    reinterpret_cast<sockaddr*>(&client),
                    &clientLen
                );

                if (fd < 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(50)
                    );
                    continue;
                }

                clientFd_ = fd;

                int sendBuffer = 128 * 1024;
                ::setsockopt(
                    clientFd_,
                    SOL_SOCKET,
                    SO_SNDBUF,
                    &sendBuffer,
                    sizeof(sendBuffer)
                );

                timeval timeout{};
                timeout.tv_sec = 0;
                timeout.tv_usec = 300000;

                ::setsockopt(
                    clientFd_,
                    SOL_SOCKET,
                    SO_SNDTIMEO,
                    &timeout,
                    sizeof(timeout)
                );
            }

            cv::Mat frame;

            {
                std::unique_lock<std::mutex> lock(mutex_);

                changed_.wait_for(
                    lock,
                    std::chrono::milliseconds(100),
                    [&] {
                        return stopping_ ||
                               version_ != sentVersion_;
                    }
                );

                if (stopping_)
                    break;

                if (version_ == sentVersion_)
                    continue;

                // Header copy only; underlying image memory is reference-counted.
                frame = latestFrame_;
                sentVersion_ = version_;
            }

            if (frame.empty())
                continue;

            cv::Mat preview;

            if (previewWidth_ > 0 && frame.cols > previewWidth_) {
                const double scale =
                    static_cast<double>(previewWidth_) / frame.cols;

                const int previewHeight = std::max(
                    1,
                    static_cast<int>(
                        std::round(frame.rows * scale)
                    )
                );

                cv::resize(
                    frame,
                    preview,
                    cv::Size(previewWidth_, previewHeight),
                    0,
                    0,
                    cv::INTER_AREA
                );
            }
            else {
                preview = frame;
            }

            jpeg.clear();

            if (!cv::imencode(
                    ".jpg",
                    preview,
                    jpeg,
                    encodeParams
                ) || jpeg.empty())
            {
                continue;
            }

            const uint32_t length =
                htonl(static_cast<uint32_t>(jpeg.size()));

            if (!sendAll(clientFd_, &length, sizeof(length)) ||
                !sendAll(clientFd_, jpeg.data(), jpeg.size()))
            {
                closeClient();
            }
        }

        closeClient();
    }

public:
    JpegFrameServer(
        int port,
        int previewWidth,
        int jpegQuality
    )
        : previewWidth_(std::max(160, previewWidth)),
          jpegQuality_(std::max(30, std::min(90, jpegQuality)))
    {
        serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);

        if (serverFd_ < 0)
            throw std::runtime_error(
                "Cannot create WinForms camera stream socket"
            );

        int reuse = 1;

        ::setsockopt(
            serverFd_,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)
        );

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port =
            htons(static_cast<uint16_t>(port));
        address.sin_addr.s_addr =
            htonl(INADDR_LOOPBACK);

        if (::bind(
                serverFd_,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)
            ) < 0)
        {
            ::close(serverFd_);
            serverFd_ = -1;

            throw std::runtime_error(
                "Cannot bind WinForms camera stream port 5000"
            );
        }

        if (::listen(serverFd_, 1) < 0) {
            ::close(serverFd_);
            serverFd_ = -1;

            throw std::runtime_error(
                "Cannot listen on WinForms camera stream port 5000"
            );
        }

        const int flags =
            ::fcntl(serverFd_, F_GETFL, 0);

        ::fcntl(
            serverFd_,
            F_SETFL,
            flags | O_NONBLOCK
        );

        worker_ =
            std::thread(&JpegFrameServer::run, this);
    }

    JpegFrameServer(const JpegFrameServer&) = delete;
    JpegFrameServer& operator=(const JpegFrameServer&) = delete;

    void publish(const cv::Mat& frame) {
        if (frame.empty())
            return;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Reference-counted assignment: no full 1280x720 deep copy here.
            latestFrame_ = frame;
            ++version_;
        }

        changed_.notify_one();
    }

    ~JpegFrameServer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }

        changed_.notify_all();

        if (worker_.joinable())
            worker_.join();

        closeClient();

        if (serverFd_ >= 0) {
            ::close(serverFd_);
            serverFd_ = -1;
        }

        latestFrame_.release();
    }
};


class AiControlServer {
    int serverFd_ = -1;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    ControlCommandChannel& commands_;

    static bool sendAll(
            int fd,
            const std::string& text) {

        const char* data = text.data();
        size_t left = text.size();

        while (left > 0) {
            const ssize_t sent =
                ::send(
                    fd,
                    data,
                    left,
                    MSG_NOSIGNAL
                );

            if (sent <= 0)
                return false;

            data += sent;
            left -= static_cast<size_t>(sent);
        }

        return true;
    }

    static std::string readCommand(int fd) {
        std::string command;
        command.reserve(64);

        char ch = 0;

        while (command.size() < 128) {
            const ssize_t n =
                ::recv(
                    fd,
                    &ch,
                    1,
                    0
                );

            if (n <= 0)
                break;

            if (ch == '\n')
                break;

            if (ch != '\r')
                command.push_back(ch);
        }

        return trimText(command);
    }

    void run() {
        while (!stopping_.load()) {
            sockaddr_in client{};
            socklen_t clientLength =
                sizeof(client);

            const int clientFd =
                ::accept(
                    serverFd_,
                    reinterpret_cast<sockaddr*>(&client),
                    &clientLength
                );

            if (clientFd < 0) {
                if (stopping_.load())
                    break;

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(50)
                );

                continue;
            }

            timeval timeout{};
            timeout.tv_sec = 6;
            timeout.tv_usec = 0;

            ::setsockopt(
                clientFd,
                SOL_SOCKET,
                SO_RCVTIMEO,
                &timeout,
                sizeof(timeout)
            );

            ::setsockopt(
                clientFd,
                SOL_SOCKET,
                SO_SNDTIMEO,
                &timeout,
                sizeof(timeout)
            );

            const std::string command =
                readCommand(clientFd);

            std::string response;

            if (command == "ADD" ||
                command == "FLUSH" ||
                command == "STOP")
            {
                // ADD/FLUSH can include image upload, so allow enough time.
                commands_.request(
                    command,
                    response,
                    120000
                );
            }
            else if (command == "RUN") {
                // AI inference is always enabled after model warm-up.
                response = "OK AI_ALREADY_RUNNING";
            }
            else if (command == "PING") {
                response = "PONG";
            }
            else {
                response =
                    "ERR UNKNOWN_COMMAND";
            }

            sendAll(
                clientFd,
                response + "\n"
            );

            ::shutdown(
                clientFd,
                SHUT_RDWR
            );

            ::close(clientFd);
        }
    }

public:
    AiControlServer(
            int port,
            ControlCommandChannel& commands)
        : commands_(commands) {

        serverFd_ =
            ::socket(
                AF_INET,
                SOCK_STREAM,
                0
            );

        if (serverFd_ < 0)
            throw std::runtime_error(
                "Cannot create AI control socket"
            );

        int reuse = 1;

        ::setsockopt(
            serverFd_,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)
        );

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port =
            htons(
                static_cast<uint16_t>(port)
            );

        address.sin_addr.s_addr =
            htonl(INADDR_LOOPBACK);

        if (::bind(
                serverFd_,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)
            ) != 0)
        {
            const int error = errno;

            ::close(serverFd_);
            serverFd_ = -1;

            throw std::runtime_error(
                "Cannot bind AI control port " +
                std::to_string(port) +
                ": " +
                std::strerror(error)
            );
        }

        if (::listen(serverFd_, 4) != 0) {
            const int error = errno;

            ::close(serverFd_);
            serverFd_ = -1;

            throw std::runtime_error(
                "Cannot listen AI control port: " +
                std::string(std::strerror(error))
            );
        }

        std::cout
            << "[CONTROL] Listening on 127.0.0.1:"
            << port
            << std::endl;

        worker_ =
            std::thread(
                &AiControlServer::run,
                this
            );
    }

    ~AiControlServer() {
        stopping_.store(true);

        if (serverFd_ >= 0) {
            ::shutdown(
                serverFd_,
                SHUT_RDWR
            );

            ::close(serverFd_);
            serverFd_ = -1;
        }

        if (worker_.joinable())
            worker_.join();
    }
};

int main() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::cerr << "[ERROR] Cannot initialize libcurl\n";
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    int returnCode = 0;

    try {
        Config c;

        // Limit OpenCV's internal CPU fan-out. We now have dedicated camera,
        // decal, error, preview and upload workers; uncontrolled OpenCV pools
        // would oversubscribe Nano's four Cortex-A57 cores.
        cv::setNumThreads(
            std::max(1, c.opencvThreads)
        );

        // Production engine names are explicit: best.engine + canluoi.engine.
        if (!std::ifstream(c.decalModel).good())
            throw std::runtime_error("Missing counting engine: " + c.decalModel);

        if (c.enableErrorModel &&
            !std::ifstream(c.errorModel).good())
            throw std::runtime_error("Missing error engine: " + c.errorModel);

        c.count.line = c.line;

        JpegFrameServer cameraStream(
            5000,
            c.previewWidth,
            c.previewJpegQuality
        );

        const auto startedAt = Clock::now();

        CameraState camera;
        AnalysisState analysis;
        std::atomic<bool> stop{false};

        ProductionManager production(c);

        if (!production.initialize()) {
            std::cerr
                << "[PRODUCTION] Initial Firebase state load failed."
                << std::endl;
        }


        // Production roll starts/resumes when WinForms sends APP_START.
        // The AI daemon itself remains persistent and independent.

        // TRUE parallel web synchronization:
        // - dedicated Firebase total thread
        // - dedicated defect JPEG/GitHub/Firebase thread
        // Both remain independent from best.engine and canluoi.engine.
        BackgroundSyncWorker syncWorker(
            c,
            production
        );

        std::cout
            << "[STARTUP] Web sync ready"
            << " | total=DEDICATED THREAD"
            << " | defect=DEDICATED THREAD"
            << std::endl;

        std::cout
            << "[MEM] rawQueue="
            << c.maxUploadQueue
            << " | diskQueue="
            << c.maxNetworkQueue
            << " | uploadMinFree="
            << c.minFreeMemoryForUploadMB
            << " MB"
            << " | perfLog="
            << (c.perfLogEnabled ? "ON" : "OFF")
            << " | OpenCVThreads="
            << c.opencvThreads
            << std::endl;

        ControlCommandChannel controlCommands;

        AiControlServer controlServer(
            c.controlPort,
            controlCommands
        );

        std::thread capture([&] {
            captureLoop(c, camera, stop, startedAt);
        });

        std::thread infer;

        if (!c.cameraOnly) {
            infer = std::thread([&] {
                inferenceLoop(
                    c,
                    camera,
                    analysis,
                    production,
                    syncWorker,
                    controlCommands,
                    stop,
                    startedAt
                );
            });
        }

        std::string fatalError;
        uint64_t lastDisplayed = 0;

        const int previewIntervalMs =
            std::max(1, 1000 / std::max(1, c.previewFps));

        Clock::time_point lastPreview{};

        while (!g_stopSignal && !stop.load()) {
            CameraFrame latest;
            bool cameraFinished;
            std::string cameraError;

            {
                std::lock_guard<std::mutex> lock(camera.mutex);

                latest = camera.newest;
                cameraFinished = camera.finished;
                cameraError = camera.error;
            }

            if (cameraFinished) {
                if (!cameraError.empty())
                    fatalError = cameraError;
                else if (latest.sequence == 0)
                    fatalError =
                        "Source opened but no frame received";

                break;
            }

            // Check AI failure without doing expensive preview work.
            bool failed = false;
            std::string aiError;

            {
                std::lock_guard<std::mutex> lock(analysis.mutex);
                failed = analysis.failed;

                if (failed)
                    aiError = analysis.error;
            }

            if (failed) {
                fatalError = aiError;
                break;
            }

            const auto now = Clock::now();

            // Preview is intentionally throttled.
            // Camera capture + both TensorRT models continue independently.
            if (lastPreview != Clock::time_point{} &&
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - lastPreview
                ).count() < previewIntervalMs)
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(5)
                );
                continue;
            }

            bool ready;
            std::vector<OverlayBox> boxes;
            size_t total;
            size_t errorsInFrame;
            size_t rawErrorsInFrame;
            cv::Mat analyzedImage;
            uint64_t analyzedSequence;
            Clock::time_point analyzedAt;

            {
                std::lock_guard<std::mutex> lock(analysis.mutex);

                ready = analysis.ready;
                boxes = analysis.boxes;
                total = analysis.total;
                errorsInFrame = analysis.errorsInFrame;
                rawErrorsInFrame = analysis.rawErrorsInFrame;
                analyzedImage = analysis.analyzedImage;
                analyzedSequence = analysis.analyzedSequence;
                analyzedAt = analysis.frameTimestamp;
            }

            const bool useAnalyzed =
                c.syncAIFrame &&
                ready &&
                analyzedSequence != 0 &&
                !analyzedImage.empty();

            const cv::Mat& source =
                useAnalyzed ? analyzedImage : latest.image;

            const uint64_t sourceSequence =
                useAnalyzed
                    ? analyzedSequence
                    : latest.sequence;

            if (sourceSequence == lastDisplayed ||
                source.empty())
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(5)
                );
                continue;
            }

            lastDisplayed = sourceSequence;
            lastPreview = now;

            // Only the preview path clones/draws.
            // AI workers keep using the untouched original frame.
            cv::Mat display = source.clone();

            if (!c.cameraOnly) {
                const auto ageMs =
                    std::chrono::duration_cast<
                        std::chrono::milliseconds
                    >(Clock::now() - analyzedAt).count();

                // Keep decal + confirmed-error bounding boxes.
                if (ready &&
                    analyzedAt != Clock::time_point{} &&
                    (useAnalyzed ||
                     ageMs <= c.maxOverlayAgeMs))
                {
                    for (const auto& box : boxes) {
                        cv::rectangle(
                            display,
                            box.rect,
                            box.color,
                            2
                        );

                        if (!box.label.empty()) {
                            cv::putText(
                                display,
                                box.label,
                                cv::Point(
                                    box.rect.x,
                                    std::max(
                                        20,
                                        box.rect.y - 10
                                    )
                                ),
                                cv::FONT_HERSHEY_SIMPLEX,
                                0.6,
                                box.color,
                                2
                            );
                        }
                    }
                }

                // Counting line is still used internally by StableCounter.
                // It is intentionally NOT drawn on the production preview.

                cv::putText(
                    display,
                    "Decal Total: " +
                        std::to_string(total),
                    cv::Point(20, 45),
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.85,
                    cv::Scalar(0, 255, 0),
                    2
                );

                cv::putText(
                    display,
                    "Error Verified: " +
                        std::to_string(errorsInFrame),
                    cv::Point(20, 80),
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.75,
                    errorsInFrame
                        ? cv::Scalar(0, 0, 255)
                        : cv::Scalar(220, 220, 220),
                    2
                );

                cv::putText(
                    display,
                    "Raw: " +
                        std::to_string(rawErrorsInFrame),
                    cv::Point(20, 112),
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.55,
                    cv::Scalar(0, 215, 255),
                    2
                );
            }

            cameraStream.publish(display);
        }

        if (g_stopSignal && !stop.load()) {
            std::string shutdownResponse;

            (void)controlCommands.request(
                "SESSION_CLOSE",
                shutdownResponse,
                15000
            );

            std::cout
                << "[SHUTDOWN] "
                << shutdownResponse
                << std::endl;
        }

        stop.store(true);
        camera.changed.notify_all();

        if (capture.joinable())
            capture.join();

        if (infer.joinable())
            infer.join();

        // Actual daemon/Jetson shutdown is also a production boundary.
        // WinForms Quit normally closes earlier via SESSION_CLOSE; this is the
        // safety net for SIGTERM/service shutdown.
        if (g_stopSignal) {
            size_t finalTotal = 0;
            {
                std::lock_guard<std::mutex> lock(analysis.mutex);
                finalTotal = analysis.total;
            }

            RollActionResult shutdownAction;
            (void)production.closeSession(
                finalTotal,
                shutdownAction,
                "SYSTEM_SHUTDOWN"
            );
        }

        if (!fatalError.empty()) {
            std::cerr
                << "[ERROR] "
                << fatalError
                << std::endl;

            returnCode = 1;
        }
    }
    catch (const std::exception& e) {
        std::cerr
            << "[ERROR] "
            << e.what()
            << std::endl;

        returnCode = 1;
    }

    curl_global_cleanup();

    // Encourage CUDA to release process-owned context resources cleanly.
    cudaDeviceSynchronize();
    cudaDeviceReset();

    return returnCode;
}