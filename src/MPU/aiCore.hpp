#pragma once

// Stable AI core for DAKTNC-TH.
// TensorRT/CUDA, tracking, ROI selection and temporal defect verification live
// here so production/database lifecycle changes do not disturb the AI pipeline.

#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/video/tracking.hpp>

#include <NvInfer.h>
#include <NvInferPlugin.h>
#include <cuda_runtime_api.h>
#include <cuda_fp16.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#if NV_TENSORRT_MAJOR < 7 || NV_TENSORRT_MAJOR >= 10
#error "This backend targets TensorRT 7/8/9 explicit-batch engines. TensorRT 10+ requires the named tensor API."
#endif

inline void cudaCheck(cudaError_t status, const char* operation) {
    if (status != cudaSuccess)
        throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(status));
}

static void setCurrentThreadNice(int niceValue) {
    const pid_t tid = static_cast<pid_t>(::syscall(SYS_gettid));
    // Increasing nice value (lower priority) is allowed for a normal user.
    // Failure is non-fatal; the pipeline still works without it.
    (void)::setpriority(PRIO_PROCESS, tid, niceValue);
}


static long systemMemAvailableMB() {
    std::ifstream input("/proc/meminfo");
    std::string key;
    long valueKb = -1;
    std::string unit;

    while (input >> key >> valueKb >> unit) {
        if (key == "MemAvailable:")
            return valueKb / 1024;
    }

    return -1;
}


class TRTLogger : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* msg) noexcept override {
        if (severity <= Severity::kWARNING) std::cerr << "[TensorRT] " << msg << std::endl;
    }
};

template<class T> struct TRTDelete {
    void operator()(T* ptr) const noexcept {
        if (!ptr) return;
#if NV_TENSORRT_MAJOR < 8
        ptr->destroy();
#else
        delete ptr;
#endif
    }
};
template<class T> using TRTHandle = std::unique_ptr<T, TRTDelete<T>>;

struct CUDAStream {
    cudaStream_t value = nullptr;

    explicit CUDAStream(bool highPriority) {
        int leastPriority = 0;
        int greatestPriority = 0;

        cudaCheck(
            cudaDeviceGetStreamPriorityRange(
                &leastPriority,
                &greatestPriority
            ),
            "cudaDeviceGetStreamPriorityRange"
        );

        const int priority =
            highPriority ? greatestPriority : leastPriority;

        cudaCheck(
            cudaStreamCreateWithPriority(
                &value,
                cudaStreamNonBlocking,
                priority
            ),
            "cudaStreamCreateWithPriority"
        );
    }

    ~CUDAStream() {
        if (value)
            cudaStreamDestroy(value);
    }

    CUDAStream(const CUDAStream&) = delete;
    CUDAStream& operator=(const CUDAStream&) = delete;
};

struct CUDABuffer {
    void* host = nullptr;
    void* device = nullptr;
    void allocate(size_t bytes) {
        cudaCheck(cudaMallocHost(&host, bytes), "cudaMallocHost");
        cudaCheck(cudaMalloc(&device, bytes), "cudaMalloc");
    }
    ~CUDABuffer() { if (device) cudaFree(device); if (host) cudaFreeHost(host); }
    CUDABuffer() = default;
    CUDABuffer(const CUDABuffer&) = delete;
    CUDABuffer& operator=(const CUDABuffer&) = delete;
};

inline size_t trtVolume(const nvinfer1::Dims& d) {
    size_t result = 1;
    for (int i = 0; i < d.nbDims; ++i) {
        if (d.d[i] <= 0 || result > std::numeric_limits<size_t>::max() / static_cast<size_t>(d.d[i]))
            throw std::runtime_error("Invalid/unresolved TensorRT dimensions");
        result *= static_cast<size_t>(d.d[i]);
    }
    return result;
}

inline size_t trtElementBytes(nvinfer1::DataType type) {
    if (type == nvinfer1::DataType::kFLOAT) return sizeof(float);
    if (type == nvinfer1::DataType::kHALF) return sizeof(__half);
    throw std::runtime_error("Only float32/float16 TensorRT I/O is supported");
}

class TensorRTBackend {
    TRTLogger logger_;
    TRTHandle<nvinfer1::IRuntime> runtime_;
    TRTHandle<nvinfer1::ICudaEngine> engine_;
    TRTHandle<nvinfer1::IExecutionContext> context_;
    CUDAStream stream_;
    CUDABuffer input_, output_;
    std::vector<void*> bindings_;
    std::vector<float> result_;
    nvinfer1::DataType inputType_, outputType_;
    size_t inputCount_ = 0, outputCount_ = 0, inputBytes_ = 0, outputBytes_ = 0;
    int width_ = 0, height_ = 0;
    int64_t candidates_ = 0;
    int channels_ = 0, classes_ = 0;
    bool channelsFirst_ = true;
public:
    TensorRTBackend(
            const std::string& path,
            int dynamicWidth,
            int dynamicHeight,
            bool highPriority)
        : stream_(highPriority) {
        if (!initLibNvInferPlugins(&logger_, ""))
            throw std::runtime_error("Cannot initialize TensorRT plugins");
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("Cannot open engine: " + path);
        auto length = file.tellg();
        if (length <= 0) throw std::runtime_error("Engine file is empty");
        std::vector<char> bytes(static_cast<size_t>(length));
        file.seekg(0);
        if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
            throw std::runtime_error("Cannot read full engine");
        // Ultralytics may prefix engine bytes with uint32 JSON length + metadata.
        // Raw TensorRT plans do not have this wrapper.
        size_t offset = 0;
        if (bytes.size() > 5) {
            uint32_t metadataLength = 0;
            std::memcpy(&metadataLength, bytes.data(), sizeof(metadataLength));
            if (metadataLength > 0 && metadataLength < bytes.size() - 4 &&
                bytes[4] == '{' && bytes[4 + metadataLength - 1] == '}') {
                offset = 4 + metadataLength;
                std::cout << "[MODEL] Removed Ultralytics metadata prefix\n";
            }
        }
        runtime_.reset(nvinfer1::createInferRuntime(logger_));
        if (!runtime_) throw std::runtime_error("Cannot create TensorRT runtime");
#if NV_TENSORRT_MAJOR < 8
        engine_.reset(runtime_->deserializeCudaEngine(bytes.data()+offset, bytes.size()-offset, nullptr));
#else
        engine_.reset(runtime_->deserializeCudaEngine(bytes.data()+offset, bytes.size()-offset));
#endif
        if (!engine_)
            throw std::runtime_error("Cannot deserialize engine. Check TensorRT version, GPU, platform and required custom plugins; build the engine on this Jetson when incompatible.");
        if (engine_->hasImplicitBatchDimension())
            throw std::runtime_error("Requires explicit-batch engine, not implicit-batch");
        if (engine_->getNbBindings() != 2 || engine_->getNbOptimizationProfiles() > 1)
            throw std::runtime_error("Requires one input, one output, and one optimization profile");
        context_.reset(engine_->createExecutionContext());
        if (!context_) throw std::runtime_error("Cannot create TensorRT context");
        int inputIndex = -1, outputIndex = -1;
        for (int i = 0; i < 2; ++i) {
            if (engine_->bindingIsInput(i)) inputIndex = i;
            else outputIndex = i;
            if (engine_->getBindingFormat(i) != nvinfer1::TensorFormat::kLINEAR ||
                engine_->getLocation(i) != nvinfer1::TensorLocation::kDEVICE)
                throw std::runtime_error("Requires linear device I/O tensors");
        }
        if (inputIndex < 0 || outputIndex < 0)
            throw std::runtime_error("Requires exactly one input and one output");
        auto inputDims = engine_->getBindingDimensions(inputIndex);
        if (inputDims.nbDims != 4) throw std::runtime_error("Expected NCHW input [1,3,H,W]");
        bool dynamic = false;
        for (int i = 0; i < 4; ++i) dynamic |= inputDims.d[i] < 0;
        if (dynamic) {
            const int sizes[] = {1,3,dynamicHeight,dynamicWidth};
            for (int i = 0; i < 4; ++i) if (inputDims.d[i] < 0) inputDims.d[i] = sizes[i];
            if (!context_->setBindingDimensions(inputIndex, inputDims))
                throw std::runtime_error("Dynamic input dimensions outside engine profile; change Config::inputWidth and Config::inputHeight");
        }
        inputDims = context_->getBindingDimensions(inputIndex);
        if (!context_->allInputDimensionsSpecified() || inputDims.d[0] != 1 || inputDims.d[1] != 3)
            throw std::runtime_error("Expected resolved input [1,3,H,W]");
        inputCount_ = trtVolume(inputDims);
        height_ = inputDims.d[2]; width_ = inputDims.d[3];
        auto out = context_->getBindingDimensions(outputIndex);
        outputCount_ = trtVolume(out);
        // Decode standard raw YOLOv8 detection output: [1,4+nc,N] or [1,N,4+nc].
        // An engine with built-in NMS, segmentation, or classification needs a different decoder.
        if (out.nbDims != 3 || out.d[0] != 1)
            throw std::runtime_error("Expected raw YOLOv8 detection output [1,4+classes,N] or [1,N,4+classes]");
        if (out.d[1] >= 5 && out.d[1] <= 256 && out.d[2] > out.d[1]) {
            channelsFirst_ = true;
            channels_ = out.d[1];
            candidates_ = out.d[2];
        } else if (out.d[2] >= 5 && out.d[2] <= 256 && out.d[1] > out.d[2]) {
            channelsFirst_ = false;
            channels_ = out.d[2];
            candidates_ = out.d[1];
        } else {
            throw std::runtime_error("Unsupported YOLOv8 output layout; check engine shape/NMS");
        }
        if (candidates_ < 500)
            throw std::runtime_error("Output looks like built-in NMS (too few candidates); export raw YOLOv8 engine with nms=False");
        classes_ = channels_ - 4;
        inputType_ = engine_->getBindingDataType(inputIndex);
        outputType_ = engine_->getBindingDataType(outputIndex);
        inputBytes_ = inputCount_ * trtElementBytes(inputType_);
        outputBytes_ = outputCount_ * trtElementBytes(outputType_);
        input_.allocate(inputBytes_); output_.allocate(outputBytes_);
        bindings_.resize(2);
        bindings_[inputIndex] = input_.device;
        bindings_[outputIndex] = output_.device;
        result_.resize(outputCount_);
        std::cout << "[MODEL] TensorRT " << NV_TENSORRT_MAJOR << "." << NV_TENSORRT_MINOR
                  << ", GPU inference; input=" << width_ << "x" << height_
                  << ", output=[" << out.d[0] << "," << out.d[1] << "," << out.d[2]
                  << "], classes=" << classes_ << "\n";
    }
    int width() const { return width_; }
    int height() const { return height_; }
    int64_t candidates() const { return candidates_; }
    bool channelsFirst() const { return channelsFirst_; }
    int channels() const { return channels_; }
    int classes() const { return classes_; }

    bool inputIsFloat() const {
        return inputType_ == nvinfer1::DataType::kFLOAT;
    }

    float* floatHostInput() {
        return inputIsFloat()
            ? static_cast<float*>(input_.host)
            : nullptr;
    }

    size_t inputCount() const {
        return inputCount_;
    }

    const std::vector<float>& executePreparedHost() {
        cudaCheck(
            cudaMemcpyAsync(
                input_.device,
                input_.host,
                inputBytes_,
                cudaMemcpyHostToDevice,
                stream_.value
            ),
            "Input upload"
        );

        if (!context_->enqueueV2(
                bindings_.data(),
                stream_.value,
                nullptr
            ))
        {
            cudaStreamSynchronize(stream_.value);
            throw std::runtime_error(
                "TensorRT enqueueV2 failed"
            );
        }

        cudaCheck(
            cudaMemcpyAsync(
                output_.host,
                output_.device,
                outputBytes_,
                cudaMemcpyDeviceToHost,
                stream_.value
            ),
            "Output download"
        );

        // This synchronizes ONLY this model's CUDA stream. The other model is
        // free to execute concurrently on its own priority stream.
        cudaCheck(
            cudaStreamSynchronize(stream_.value),
            "TensorRT stream synchronize"
        );

        if (outputType_ == nvinfer1::DataType::kFLOAT) {
            std::memcpy(
                result_.data(),
                output_.host,
                outputBytes_
            );
        }
        else {
            auto src =
                static_cast<const __half*>(output_.host);

            for (size_t i = 0;
                 i < outputCount_;
                 ++i)
            {
                result_[i] =
                    __half2float(src[i]);
            }
        }

        return result_;
    }

    const std::vector<float>& runPreparedFloatHost(
            size_t count) {

        if (!inputIsFloat() || count != inputCount_) {
            throw std::runtime_error(
                "Prepared pinned-host input mismatch"
            );
        }

        return executePreparedHost();
    }

    const std::vector<float>& run(
            const float* data,
            size_t count) {

        if (count != inputCount_) {
            throw std::runtime_error(
                "Input element count mismatch"
            );
        }

        if (inputType_ == nvinfer1::DataType::kFLOAT) {
            std::memcpy(
                input_.host,
                data,
                inputBytes_
            );
        }
        else {
            auto dst =
                static_cast<__half*>(input_.host);

            for (size_t i = 0;
                 i < count;
                 ++i)
            {
                dst[i] = __float2half(data[i]);
            }
        }

        return executePreparedHost();
    }
};


#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <string>

struct CountSettings {
    int line = 480, margin = 15, minHits = 4, sideFrames = 2, maxGap = 8;
    std::string direction = "right"; //CAN BE ADJUSTED
};

// Observe measured/associated tracks only. Never count a held display box.
class StableCounter {
    struct State {
        size_t lastFrame = 0;
        int hits = 0, candidateSide = 0, sideHits = 0, stableSide = 0;
    };
    CountSettings settings_;
    std::unordered_map<size_t, State> states_;
    std::unordered_set<size_t> counted_;
    size_t total_ = 0;
public:
    explicit StableCounter(CountSettings settings) : settings_(settings) {}
    bool observe(size_t id, double cy, size_t frame) {
        auto& state = states_[id];
        if (state.lastFrame == frame) return false;
        size_t gap = state.lastFrame ? frame - state.lastFrame : 0;
        if (gap > static_cast<size_t>(settings_.maxGap)) state = State{};
        if (gap > 1) { state.candidateSide = 0; state.sideHits = 0; }
        state.hits += 1;
        state.lastFrame = frame;
        int side = cy < settings_.line - settings_.margin ? -1 :
                   cy > settings_.line + settings_.margin ? 1 : 0;
        if (side == 0) {
            state.candidateSide = 0;
            state.sideHits = 0;
            return false;
        }
        if (state.candidateSide != side) { state.candidateSide = side; state.sideHits = 1; }
        else ++state.sideHits;
        if (state.sideHits < settings_.sideFrames) return false;
        if (state.stableSide == 0) { state.stableSide = side; return false; }
        if (state.stableSide == side) return false;
        const int oldSide = state.stableSide;
        bool allowed = settings_.direction == "both" ||
            ((settings_.direction == "right" || settings_.direction == "down") && oldSide == -1 && side == 1) ||
            ((settings_.direction == "left" || settings_.direction == "up") && oldSide == 1 && side == -1);
        if (allowed && state.hits < settings_.minHits) return false;
        state.stableSide = side;
        if (state.hits >= settings_.minHits && allowed && counted_.insert(id).second) {
            ++total_;
            return true;
        }
        return false;
    }
    bool confirmed(size_t id) const {
        auto it = states_.find(id);
        return it != states_.end() && it->second.hits >= settings_.minHits;
    }
    bool counted(size_t id) const { return counted_.count(id) != 0; }
    size_t total() const { return total_; }
    void reset() {
        std::unordered_map<size_t, State>().swap(states_);
        std::unordered_set<size_t>().swap(counted_);
        total_ = 0;
    }
    void prune(size_t frame) {
        for (auto it = states_.begin(); it != states_.end();) {
            if (frame - it->second.lastFrame > 300) {
                // Track IDs are never reused inside a roll. Once a track has
                // been absent for 300 AI frames, retaining it in counted_ only
                // creates monotonic memory growth on long production runs.
                counted_.erase(it->first);
                it = states_.erase(it);
            }
            else ++it;
        }
    }
};


#include <opencv2/video/tracking.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

// Local Kalman + global assignment tracker, not the ByteTrack library.
namespace local_track {
template<class T> struct Rect {
    T px, py, pw, ph;
    Rect(T x, T y, T w, T h) : px(x), py(y), pw(w), ph(h) {}
    T x() const { return px; } T y() const { return py; }
    T br_x() const { return px+pw; } T br_y() const { return py+ph; }
};
struct Object {
    Rect<float> rect;
    int label;
    float prob;
    Object(Rect<float> r, int c, float s) : rect(r),label(c),prob(s) {}
};

// Rectangular Hungarian solver. Every row has its own dummy column.
std::vector<int> assignMinimum(const std::vector<std::vector<double>>& cost) {
    int n=static_cast<int>(cost.size());
    if (!n) return {};
    int m=static_cast<int>(cost[0].size());
    std::vector<double> u(n+1),v(m+1);
    std::vector<int> p(m+1),way(m+1);
    for(int i=1;i<=n;++i) {
        p[0]=i; int j0=0;
        std::vector<double> best(m+1,1e12);
        std::vector<bool> used(m+1,false);
        do {
            used[j0]=true; int i0=p[j0],j1=0; double delta=1e12;
            for(int j=1;j<=m;++j) if(!used[j]) {
                double cur=cost[i0-1][j-1]-u[i0]-v[j];
                if(cur<best[j]) {best[j]=cur;way[j]=j0;}
                if(best[j]<delta) {delta=best[j];j1=j;}
            }
            for(int j=0;j<=m;++j) {
                if(used[j]) {u[p[j]]+=delta;v[j]-=delta;}
                else best[j]-=delta;
            }
            j0=j1;
        } while(p[j0]!=0);
        do {int j1=way[j0];p[j0]=p[j1];j0=j1;} while(j0);
    }
    std::vector<int> result(n,-1);
    for(int j=1;j<=m;++j) if(p[j]) result[p[j]-1]=j-1;
    return result;
}

class Track {
    cv::KalmanFilter filter_{8,4,0,CV_32F};
    Rect<float> rect_{0,0,1,1};
    size_t id_;
public:
    int missed=0;
    explicit Track(size_t id, const Object& d) : id_(id) {
        filter_.transitionMatrix=cv::Mat::eye(8,8,CV_32F);
        for(int i=0;i<4;++i) filter_.transitionMatrix.at<float>(i,i+4)=1;
        filter_.measurementMatrix=cv::Mat::zeros(4,8,CV_32F);
        for(int i=0;i<4;++i) filter_.measurementMatrix.at<float>(i,i)=1;
        cv::setIdentity(filter_.processNoiseCov,cv::Scalar(1.0));
        cv::setIdentity(filter_.measurementNoiseCov,cv::Scalar(4.0));
        cv::setIdentity(filter_.errorCovPost,cv::Scalar(10.0));
        filter_.statePost=cv::Mat::zeros(8,1,CV_32F);
        filter_.statePost.at<float>(0)=d.rect.x()+d.rect.pw/2;
        filter_.statePost.at<float>(1)=d.rect.y()+d.rect.ph/2;
        filter_.statePost.at<float>(2)=d.rect.pw;
        filter_.statePost.at<float>(3)=d.rect.ph;
        rect_=d.rect;
    }
    const Rect<float>& getRect() const { return rect_; }
    size_t getTrackId() const { return id_; }
    void setRect(const cv::Mat& state) {
        float w=std::max(1.f,state.at<float>(2)), h=std::max(1.f,state.at<float>(3));
        rect_=Rect<float>(state.at<float>(0)-w/2,state.at<float>(1)-h/2,w,h);
    }
    void predict() { setRect(filter_.predict());++missed; }
    void correct(const Object& d) {
        cv::Mat measurement=(cv::Mat_<float>(4,1)<<d.rect.x()+d.rect.pw/2,
            d.rect.y()+d.rect.ph/2,d.rect.pw,d.rect.ph);
        setRect(filter_.correct(measurement));missed=0;
    }
};

class Tracker {
    std::vector<std::shared_ptr<Track>> tracks_;
    size_t nextId_=1;
    int maxLost_;
    float strong_, newTrack_;
    static double cost(const Rect<float>& a,const Rect<float>& b) {
        if(a.pw<=0 || a.ph<=0 || b.pw<=0 || b.ph<=0) return 1e4;
        double sizeRatio=std::max({a.pw/b.pw,b.pw/a.pw,a.ph/b.ph,b.ph/a.ph});
        if(sizeRatio>2.5) return 1e4;
        double w=std::max(0.f,std::min(a.br_x(),b.br_x())-std::max(a.x(),b.x()));
        double h=std::max(0.f,std::min(a.br_y(),b.br_y())-std::max(a.y(),b.y()));
        double inter=w*h, iou=inter/(a.pw*a.ph+b.pw*b.ph-inter);
        double dx=(a.x()+a.pw/2)-(b.x()+b.pw/2);
        double dy=(a.y()+a.ph/2)-(b.y()+b.ph/2);
        //double scale=std::max(10.0,std::hypot((a.pw+b.pw)/2,(a.ph+b.ph)/2));
        double scale=std::max<double>(10.0,std::hypot((a.pw+b.pw)/2,(a.ph+b.ph)/2));
        double distance=std::hypot(dx,dy)/scale;
        if(iou<0.05 && distance>0.8) return 1e4;
        return 0.7*(1-iou)+0.3*std::min(distance,1.0);
    }
public:
    Tracker(int maxLost,float strong,float newTrack)
        : maxLost_(maxLost),strong_(strong),newTrack_(newTrack) {}
    void reset() {
        std::vector<std::shared_ptr<Track>>().swap(tracks_);
        nextId_ = 1;
    }
    std::vector<std::shared_ptr<Track>> update(const std::vector<Object>& objects) {
        for(auto& t:tracks_)t->predict();
        std::vector<bool> matchedTrack(tracks_.size(),false), matchedObject(objects.size(),false);
        // First strong observations; then weak observations can maintain recent tracks.
        for(int pass=0;pass<2;++pass) {
            std::vector<size_t> rows,cols;
            for(size_t i=0;i<tracks_.size();++i)
                if(!matchedTrack[i] && tracks_[i]->missed<=maxLost_ &&
                    (pass==0 || tracks_[i]->missed<=5)) rows.push_back(i);
            for(size_t j=0;j<objects.size();++j)
                if(!matchedObject[j] && ((objects[j].prob>=strong_)==(pass==0)))cols.push_back(j);
            if(rows.empty() || cols.empty())continue;
            std::vector<std::vector<double>> costs(rows.size(),std::vector<double>(cols.size()+rows.size(),0.96));
            for(size_t i=0;i<rows.size();++i)
                for(size_t j=0;j<cols.size();++j)
                    costs[i][j]=cost(tracks_[rows[i]]->getRect(),objects[cols[j]].rect);
            auto assigned=assignMinimum(costs);
            for(size_t i=0;i<rows.size();++i) {
                int j=assigned[i];
                if(j<0 || static_cast<size_t>(j)>=cols.size() || costs[i][j]>=0.95)continue;
                tracks_[rows[i]]->correct(objects[cols[j]]);
                matchedTrack[rows[i]]=true;matchedObject[cols[j]]=true;
            }
        }
        for(size_t j=0;j<objects.size();++j)
            if(!matchedObject[j] && objects[j].prob>=newTrack_)
                tracks_.push_back(std::make_shared<Track>(nextId_++,objects[j]));
        tracks_.erase(std::remove_if(tracks_.begin(),tracks_.end(),
            [&](const auto& t){return t->missed>maxLost_;}),tracks_.end());
        std::vector<std::shared_ptr<Track>> visible;
        for(const auto& t:tracks_)if(t->missed==0)visible.push_back(t);
        return visible;
    }
};
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ===== USB CAMERA CONFIGURATION: EDIT HERE =====
// cameraIndex=0 corresponds to /dev/video0 (verify using v4l2-ctl --list-devices).
// cameraOnly=true: open camera only; false: detection + tracking + counting.
// direction=left: count from right to left; margin: buffer zone to prevent jitter/false triggers.
struct Config {
    std::string decalModel = "Write your Count Model Path"; // Production counting engine.
    std::string errorModel = "Write you Error Model Path"; // Production defect engine.
    int cameraIndex = 0;                 // USB webcam: /dev/video0
    int cameraFlip = -1;                  // -1 = xoay 180 do, 1 = lat ngang, 0 = lat doc, 2 = khong lat
    int width = 1280, height = 720, fps = 30;
    bool useMJPG = true;                // Camera nao khong ho tro MJPG: doi thanh false
    int line = 480;
    std::string axis = "x";
    int inputWidth = 640, inputHeight = 640;
    float confidence = 0.35f, lowConfidence = 0.1f, newConfidence = 0.5f, iou = 0.45f;
    // Two-stage error decision: weak boxes go into tracking, NOT straight to NG.
    float errorConfidence = 0.45f;  // Candidate confidence: feed temporal tracker.
    float errorConfirmConfidence = 0.65f; // Strong observation if >= this.
    float errorWeakStableConfidence = 0.65f; // Weak-only 4/5 needs EMA >= this.
    // Raw detections are still counted internally, but raw yellow diagnostic
    // boxes are not drawn in production view.
    float errorIou = 0.45f;
    int maxErrorBoxes = 20;
    // Error ROI: decal bounding box + padding; 2 horizontal overlapping tiles.
    bool errorUseDecalROI = true;
    bool errorTwoTiles = false;        // Set false to benchmark 1x error inference.
    float errorROIPadding = 0.025f;  // Pad 2.5% of image size around decal.
    float errorTileFraction = 0.62f; // Two tiles overlap by 24% of ROI width.
    // Reject unusually LARGE error boxes in a model trained mostly on small defects.
    // Fractions refer to the selected decal ROI, NOT the full camera frame.
    float errorMaxWidthRatio = 0.22f;
    float errorMaxHeightRatio = 0.26f;
    float errorMaxAreaRatio = 0.025f;
    bool errorSingleDecalROI = true; // Track exactly one decal near counting line.
    // Tracking only on AI-processed frames. 3 detections / last 5 processed frames.
    int errorConfirmHits = 4;        // If conveyor too fast, trial 2 (more FP risk).
    int errorWindow = 6;
    int errorMaxMissed = 1;       // Preserve a confirmed track briefly (AI frames).
    bool syncAIFrame = false;       // APP: false = live camera smooth; overlay only while fresh.
    bool enableErrorModel = true;
    CountSettings count;
    int holdFrames = 4;
    bool letterbox = true;
    bool cameraOnly = false;
    int maxOverlayAgeMs = 350; // Only used if syncAIFrame=false.

    // WinForms preview ONLY. AI still receives the original 1280x720 frame.
    int previewWidth = 512;       // UI/VNC only; AI still receives 1280x720.
    int previewFps = 5;
    int previewJpegQuality = 55;

    // Jetson Nano performance budget.
    int opencvThreads = 1;       // Avoid oversubscribing 4 Cortex-A57 cores.
    int errorWorkerNice = 3;       // Error preprocess/inference yields to counting.
    int totalWorkerNice = 6;       // Lightweight Firebase total updater.
    int uploadWorkerNice = 12;     // JPEG/Base64/GitHub/Firebase defect worker lowest CPU priority.
    size_t maxUploadQueue = 2;      // RAW defect images waiting for JPEG encode. Hard RAM bound.
    size_t maxNetworkQueue = 64;     // Disk-backed metadata jobs; no cv::Mat held here.
    int minFreeMemoryForUploadMB = 64;  // Emergency floor only; disk-backed upload no longer needs 350 MB free.
    bool perfLogEnabled = false;      // Persistent main stays quiet unless profiling is enabled.

    // Verified defect capture.
    // Raw detections NEVER trigger a save.
    std::string defectCaptureDir = "error_captures";
    int defectCaptureJpegQuality = 88;

    // After both VERIFIED and decal position are known, keep observing a few
    // AI frames so a sharper/full-decal image can replace the first candidate.
    int defectFinalizeDelayFrames = 3; // Keep a few frames for sharpness without delaying realtime upload too long.

    // Extra production gate AFTER TemporalErrorTracker VERIFIED:
    // a physical defect region must be seen in the same normalized position
    // for at least 3 of the last 5 real AI observations before it is allowed
    // into a saved evidence image.
    int defectConsensusWindow = 5;
    int defectConsensusHits = 3;
    float defectConsensusMinConfidence = 0.68f;

    int defectStateKeepFrames = 120; // Normal inactive-state retention.
    int defectOrphanKeepFrames = 240; // VERIFIED but unqueueable state may never live forever.
    int memoryLogSeconds = 30;         // Low-rate RSS telemetry for long industrial runs.

    // ============================================================
    // GITHUB IMAGE UPLOAD
    // Token is stored outside source code:
    //   /home/jetson/.config/decal/github_token
    // Fine-grained PAT permission: Contents = Read and write.
    // ============================================================
    bool githubUploadEnabled = true;
    std::string githubOwner = "devtdn2005";
    std::string githubRepo = "DecalDetection-WebServer";
    std::string githubBranch = "main";
    std::string githubTokenFile = "/home/jetson/.config/decal/github_token";
    std::string githubImageRoot = "images";

    // Final production database.
    std::string firebaseBaseUrl =
        "https://decalwebserver-default-rtdb.asia-southeast1.firebasedatabase.app";
    std::string productionRoot = "ProductionByDate";
    std::string productionRuntimeRoot = "ProductionRuntime";

    // WinForms -> persistent C++ control channel.
    int controlPort = 5001;

    // Verified dedup is implemented geometrically in normalized decal space.
    // Current defaults:
    //   IoU >= 0.30 -> same defect
    //   OR center distance <= 2.5% of decal and size ratio <= 2.5.
};

class Detector {
    TensorRTBackend backend_;
    float confidence_, iou_;
    bool letterbox_;

    // Reused preprocessing buffers. Each Detector belongs to exactly one worker
    // thread, so these buffers never need a mutex.
    cv::Mat resized_;
    cv::Mat bordered_;
    cv::Mat blob_;

public:
    Detector(
            const Config& c,
            const std::string& enginePath,
            float confidence,
            float iou,
            bool highPriority = true)
        : backend_(
              enginePath,
              c.inputWidth,
              c.inputHeight,
              highPriority
          ),
          confidence_(confidence),
          iou_(iou),
          letterbox_(c.letterbox) {}

    std::vector<local_track::Object> detect(const cv::Mat& frame) {
        cv::Mat prepared;
        float sx = static_cast<float>(frame.cols) / backend_.width();
        float sy = static_cast<float>(frame.rows) / backend_.height();
        int left = 0, top = 0;
        if (letterbox_) {
            double scale = std::min(static_cast<double>(backend_.width()) / frame.cols,
                                    static_cast<double>(backend_.height()) / frame.rows);
            int rw = std::max(1, static_cast<int>(std::round(frame.cols * scale)));
            int rh = std::max(1, static_cast<int>(std::round(frame.rows * scale)));
            left = (backend_.width() - rw) / 2;
            top = (backend_.height() - rh) / 2;
            cv::resize(
                frame,
                resized_,
                cv::Size(rw, rh),
                0,
                0,
                cv::INTER_LINEAR
            );

            cv::copyMakeBorder(
                resized_,
                bordered_,
                top,
                backend_.height()-rh-top,
                left,
                backend_.width()-rw-left,
                cv::BORDER_CONSTANT,
                cv::Scalar(114,114,114)
            );

            prepared = bordered_;
            sx = static_cast<float>(frame.cols) / rw;
            sy = static_cast<float>(frame.rows) / rh;
        }
        else {
            prepared = frame;
        }

        const std::vector<float>* outputPtr = nullptr;

        if (backend_.inputIsFloat() &&
            backend_.floatHostInput() != nullptr)
        {
            const int blobSizes[4] = {
                1,
                3,
                backend_.height(),
                backend_.width()
            };

            cv::Mat pinnedBlob(
                4,
                blobSizes,
                CV_32F,
                backend_.floatHostInput()
            );

            cv::dnn::blobFromImage(
                prepared,
                pinnedBlob,
                1.0 / 255.0,
                cv::Size(
                    backend_.width(),
                    backend_.height()
                ),
                cv::Scalar(),
                true,
                false,
                CV_32F
            );

            // OpenCV normally reuses the provided correctly-sized buffer.
            // If a particular OpenCV build reallocates it, fall back safely.
            if (pinnedBlob.data ==
                reinterpret_cast<unsigned char*>(
                    backend_.floatHostInput()
                ))
            {
                outputPtr =
                    &backend_.runPreparedFloatHost(
                        backend_.inputCount()
                    );
            }
            else {
                outputPtr =
                    &backend_.run(
                        pinnedBlob.ptr<float>(),
                        pinnedBlob.total()
                    );
            }
        }
        else {
            cv::dnn::blobFromImage(
                prepared,
                blob_,
                1.0 / 255.0,
                cv::Size(
                    backend_.width(),
                    backend_.height()
                ),
                cv::Scalar(),
                true,
                false,
                CV_32F
            );

            outputPtr =
                &backend_.run(
                    blob_.ptr<float>(),
                    blob_.total()
                );
        }

        const auto& output = *outputPtr;
        const int64_t n = backend_.candidates();
        const int channels = backend_.channels();
        auto at = [&](int64_t row, int col) {
            return backend_.channelsFirst() ? output[static_cast<size_t>(col)*n + row]
                                            : output[static_cast<size_t>(row)*channels + col];
        };
        // Per-class NMS (compatible with older OpenCV on JetPack 4).
        std::vector<cv::Rect> boxes;
        std::vector<float> scores;
        std::vector<int> labels;
        for (int64_t i=0; i<n; ++i) {
            float bestScore=-1.f;
            int bestClass=-1;
            for (int cls=0; cls<backend_.classes(); ++cls) {
                float value = at(i, 4+cls);
                if (std::isfinite(value) && value > bestScore) {
                    bestScore=value;
                    bestClass=cls;
                }
            }
            if (bestScore < confidence_ || bestClass < 0) continue;
            float x=at(i,0), y=at(i,1), w=at(i,2), h=at(i,3);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) ||
                !std::isfinite(h) || w <= 0 || h <= 0) continue;
            int bw=static_cast<int>(w*sx), bh=static_cast<int>(h*sy);
            if (bw <= 0 || bh <= 0) continue;
            cv::Rect box(static_cast<int>((x-w*0.5f-left)*sx),
                         static_cast<int>((y-h*0.5f-top)*sy), bw, bh);
            box &= cv::Rect(0,0,frame.cols,frame.rows);
            if (box.area() <= 0) continue;
            boxes.push_back(box);
            scores.push_back(bestScore);
            labels.push_back(bestClass);
        }
        std::vector<local_track::Object> detected;
        for (int cls=0; cls<backend_.classes(); ++cls) {
            std::vector<cv::Rect> classBoxes;
            std::vector<float> classScores;
            for (size_t i=0; i<boxes.size(); ++i) {
                if (labels[i] == cls) {
                    classBoxes.push_back(boxes[i]);
                    classScores.push_back(scores[i]);
                }
            }
            if (classBoxes.empty()) continue;
            std::vector<int> keep;
            cv::dnn::NMSBoxes(classBoxes, classScores, confidence_, iou_, keep);
            for (int index : keep) {
                const cv::Rect& b = classBoxes[index];
                detected.emplace_back(local_track::Rect<float>(b.x,b.y,b.width,b.height),
                                      cls, classScores[index]);
            }
        }
        std::sort(detected.begin(), detected.end(),
                  [](const local_track::Object& a, const local_track::Object& b) {
                      return a.prob > b.prob;
                  });
        return detected;
    }
};


// ===== ERROR ROI + MULTI-TILE INFERENCE =====
// Select ONE consistently tracked decal for error analysis. Previous R3 merged
// all visible decals; relative error coordinates then jumped when one entered/
// exited the scene. This selector uses the persistent DECAL track ID.
struct ErrorROISelection {
    cv::Rect roi;
    cv::Rect decalBox;
    size_t id = 0;
    bool fresh = false;
};

class StableErrorROI {
    size_t activeId_ = 0;
    cv::Rect lastBox_;
    cv::Rect2f smoothBox_;
    int lost_ = 0;
public:
    void reset() {
        activeId_ = 0;
        lastBox_ = cv::Rect();
        smoothBox_ = cv::Rect2f();
        lost_ = 0;
    }

    ErrorROISelection select(const Config& c, const cv::Size& size,
                             const std::vector<std::shared_ptr<local_track::Track>>& visible) {
        const cv::Rect limits(0, 0, size.width, size.height);
        if (!c.errorUseDecalROI || !c.errorSingleDecalROI)
            return {limits, limits, 1, true}; // Diagnostic whole-image mode.
        const double minArea = 0.04 * static_cast<double>(size.area());
        const local_track::Track* winner = nullptr;
        double best = 1e20;
        for (const auto& ptr : visible) {
            const auto& r = ptr->getRect();
            cv::Rect b(cvRound(r.x()),cvRound(r.y()),
                       std::max(1,cvRound(r.pw)),std::max(1,cvRound(r.ph)));
            b &= limits;
            if (b.empty() || b.area() < minArea) continue;
            
            const double cx = b.x + b.width * 0.5;
            // Prefer a label around the line, but keep the currently tracked
            // label until a competitor is distinctly better / closer.
            double cost = std::abs(cx - c.line) / size.width;
            if (ptr->getTrackId() == activeId_) cost -= 0.11;
            if (cost < best) {best = cost; winner = ptr.get();}
        }
        if (winner) {
            const auto& r = winner->getRect();
            const size_t winnerId = winner->getTrackId();
            const cv::Rect2f current(r.x(),r.y(),std::max(1.f,r.pw),std::max(1.f,r.ph));
            if (winnerId != activeId_ || smoothBox_.empty()) {
                smoothBox_ = current;
            } else {
                // Moderate ROI smoothing; fast enough to follow moving label.
                const float alpha = 0.65f;
                smoothBox_.x = (1-alpha)*smoothBox_.x + alpha*current.x;
                smoothBox_.y = (1-alpha)*smoothBox_.y + alpha*current.y;
                smoothBox_.width = (1-alpha)*smoothBox_.width + alpha*current.width;
                smoothBox_.height = (1-alpha)*smoothBox_.height + alpha*current.height;
            }
            activeId_ = winnerId;
            lastBox_ = cv::Rect(cvRound(smoothBox_.x),cvRound(smoothBox_.y),
                                std::max(1,cvRound(smoothBox_.width)),
                                std::max(1,cvRound(smoothBox_.height))) & limits;
            lost_ = 0;
        } else if (activeId_ && ++lost_ > 2) {
            activeId_ = 0;
            lastBox_ = cv::Rect();
            smoothBox_ = cv::Rect2f();
        }
        if (!activeId_ || lastBox_.empty()) return {};
        const int padX = cvRound(size.width * c.errorROIPadding);
        const int padY = cvRound(size.height * c.errorROIPadding);
        cv::Rect expanded(lastBox_.x-padX,lastBox_.y-padY,
                          lastBox_.width+2*padX,lastBox_.height+2*padY);
        return {expanded & limits, lastBox_, activeId_, winner!=nullptr};
    }
};

static std::vector<cv::Rect> errorTiles(cv::Rect roi, const Config& c) {
    if (!c.errorTwoTiles || roi.width < 100) return {roi};
    const float fraction = std::max(0.51f, std::min(0.85f, c.errorTileFraction));
    const int w = std::min(roi.width, std::max(1, cvRound(roi.width * fraction)));
    return {cv::Rect(roi.x, roi.y, w, roi.height),
            cv::Rect(roi.x + roi.width - w, roi.y, w, roi.height)};
}

static std::vector<local_track::Object> detectErrorsInROI(
        Detector& detector, const cv::Mat& frame, const Config& c,
        const cv::Rect& roi) {
    std::vector<local_track::Object> candidates;
    for (const cv::Rect& tile : errorTiles(roi, c)) {
        // OpenCV ROI is a view, not an extra copy of the large camera image.
        const auto detections = detector.detect(frame(tile));
        for (const auto& d : detections) {
            const cv::Rect r(cvRound(d.rect.x()) + tile.x,
                             cvRound(d.rect.y()) + tile.y,
                             cvRound(d.rect.pw), cvRound(d.rect.ph));
            if (r.area() <= 0) continue;
            candidates.emplace_back(local_track::Rect<float>(r.x, r.y, r.width, r.height),
                                    d.label, d.prob);
        }
    }
    // Merge duplicate boxes originating from overlapping tiles; per class.
    std::vector<local_track::Object> output;
    std::unordered_set<int> classes;
    for (const auto& d : candidates) classes.insert(d.label);
    for (int cls : classes) {
        std::vector<cv::Rect> boxes;
        std::vector<float> scores;
        for (const auto& d : candidates) {
            if (d.label != cls) continue;
            boxes.emplace_back(cvRound(d.rect.x()), cvRound(d.rect.y()),
                               cvRound(d.rect.pw), cvRound(d.rect.ph));
            scores.push_back(d.prob);
        }
        std::vector<int> keep;
        if (!boxes.empty())
            cv::dnn::NMSBoxes(boxes, scores, c.errorConfidence, c.errorIou, keep);
        for (int k : keep) {
            const cv::Rect& b = boxes[k];
            output.emplace_back(local_track::Rect<float>(b.x, b.y, b.width, b.height),
                                cls, scores[k]);
        }
    }
    // Reject boxes far larger than defect labels. Large false boxes across
    // words / fox drawings repeatedly became CONFIRMED in the R3 video.
    // Also require the box center to fall inside the decal (not its padding).
    output.erase(std::remove_if(output.begin(), output.end(), [&](const local_track::Object& d) {
        const float bw = d.rect.pw, bh = d.rect.ph;
        const float fracW = bw / std::max(1,roi.width);
        const float fracH = bh / std::max(1,roi.height);
        const float fracArea = (bw*bh) / std::max(1,roi.area());
        return fracW > c.errorMaxWidthRatio ||
               fracH > c.errorMaxHeightRatio ||
               fracArea > c.errorMaxAreaRatio;
    }), output.end());
    std::sort(output.begin(), output.end(),
              [](const local_track::Object& a, const local_track::Object& b) {
                  return a.prob > b.prob;
              });
    // Cap candidate count to avoid tracking stalls from many false positives.
    if (output.size() > 100) output.erase(output.begin()+100, output.end());
    return output;
}

// ============================================================
// ASYNC ERROR MODEL WORKER
//
// The counting model never waits for Error Model. The worker keeps at most one
// pending job; if camera/decal tracking advances while er1 is busy, the pending
// job is replaced by the newest ROI instead of building latency.
// ============================================================
class AsyncErrorWorker {
public:
    struct Result {
        uint64_t epoch = 0;
        uint64_t cameraSequence = 0;
        uint64_t aiFrame = 0;
        size_t decalId = 0;
        cv::Rect roi;
        cv::Rect decalBox;
        cv::Mat frame;
        std::vector<local_track::Object> raw;
        double processingMs = 0.0;
    };

private:
    struct Job {
        uint64_t epoch = 0;
        uint64_t cameraSequence = 0;
        uint64_t aiFrame = 0;
        size_t decalId = 0;
        cv::Rect roi;
        cv::Rect decalBox;
        cv::Mat frame;
    };

    const Config& config_;
    Detector detector_;

    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread worker_;

    Job pending_;
    bool hasPending_ = false;

    Result latest_;
    bool hasResult_ = false;

    bool stopping_ = false;

    void run() {
        setCurrentThreadNice(config_.errorWorkerNice);

        while (true) {
            Job job;

            {
                std::unique_lock<std::mutex> lock(mutex_);

                changed_.wait(
                    lock,
                    [&] {
                        return stopping_ || hasPending_;
                    }
                );

                if (stopping_)
                    return;

                job = std::move(pending_);
                pending_ = Job{};
                hasPending_ = false;
            }

            Result result;
            result.epoch = job.epoch;
            result.cameraSequence = job.cameraSequence;
            result.aiFrame = job.aiFrame;
            result.decalId = job.decalId;
            result.roi = job.roi;
            result.decalBox = job.decalBox;
            result.frame = job.frame;

            const auto inferStarted =
                std::chrono::steady_clock::now();

            result.raw = detectErrorsInROI(
                detector_,
                job.frame,
                config_,
                job.roi
            );

            result.processingMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - inferStarted
                ).count();

            // Padding is allowed for inference, but a final candidate must
            // still belong to the physical decal.
            result.raw.erase(
                std::remove_if(
                    result.raw.begin(),
                    result.raw.end(),
                    [&](const local_track::Object& d) {
                        const cv::Point2f center(
                            d.rect.x() + d.rect.pw * 0.5f,
                            d.rect.y() + d.rect.ph * 0.5f
                        );

                        return !job.decalBox.contains(
                            cv::Point(
                                cvRound(center.x),
                                cvRound(center.y)
                            )
                        );
                    }
                ),
                result.raw.end()
            );

            {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_ = std::move(result);
                hasResult_ = true;
            }
        }
    }

public:
    explicit AsyncErrorWorker(const Config& config)
        : config_(config),
          detector_(
              config,
              config.errorModel,
              config.errorConfidence,
              config.errorIou,
              false
          )
    {
        // Warm the low-priority execution context before production starts.
        const cv::Mat warmup(
            std::max(1, config.inputHeight),
            std::max(1, config.inputWidth),
            CV_8UC3,
            cv::Scalar(114,114,114)
        );

        (void)detector_.detect(warmup);

        worker_ = std::thread(
            &AsyncErrorWorker::run,
            this
        );
    }

    ~AsyncErrorWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            hasPending_ = false;
        }

        changed_.notify_all();

        if (worker_.joinable())
            worker_.join();
    }

    void publishLatest(
            uint64_t epoch,
            uint64_t cameraSequence,
            uint64_t aiFrame,
            size_t decalId,
            const cv::Rect& roi,
            const cv::Rect& decalBox,
            const cv::Mat& frame) {

        if (decalId == 0 || roi.empty() || frame.empty())
            return;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Latest-only queue: overwrite stale pending work.
            pending_.epoch = epoch;
            pending_.cameraSequence = cameraSequence;
            pending_.aiFrame = aiFrame;
            pending_.decalId = decalId;
            pending_.roi = roi;
            pending_.decalBox = decalBox;
            pending_.frame = frame; // ref-counted; capture buffer remains alive.
            hasPending_ = true;
        }

        changed_.notify_one();
    }

    bool tryTakeLatest(Result& result) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!hasResult_)
            return false;

        result = std::move(latest_);
        hasResult_ = false;
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = Job{};
        latest_ = Result{};
        hasPending_ = false;
        hasResult_ = false;
    }
};


// ===== TEMPORAL TRACKER FOR SMALL ERRORS =====
// Track in normalized decal ROI coordinates instead of raw screen pixels. This
// compensates approximately for the decal moving as a group on the conveyor.
// Distinct error boxes use one-to-one Hungarian assignment.
class TemporalErrorTracker {
    struct Item {
        size_t id = 0;
        int cls = 0;
        cv::Rect2f norm;  // Relative to the current decal ROI: [0, 1].
        float confidence = 0;
        unsigned int history = 0; // Sliding observation bitmask.
        unsigned int strongHistory = 0; // Observations passing strong threshold.
        int missed = 0;
        bool latched = false;  // Once confirmed, keep until track is really lost.
    };
    std::vector<Item> items_;
    size_t nextId_ = 1;
    int window_;
    int minHits_;
    int maxMissed_;
    float strongThreshold_;
    float weakStableThreshold_;

    static cv::Rect2f normalize(const local_track::Object& d, const cv::Rect& roi) {
        return cv::Rect2f((d.rect.x()-roi.x)/roi.width,
                          (d.rect.y()-roi.y)/roi.height,
                          d.rect.pw/roi.width, d.rect.ph/roi.height);
    }
    static double boxIoU(const cv::Rect2f& a, const cv::Rect2f& b) {
        const cv::Rect2f inter = a & b;
        const double unionArea = a.area() + b.area() - inter.area();
        return unionArea > 0 ? inter.area()/unionArea : 0.0;
    }
    static double matchCost(const Item& a, const cv::Rect2f& b, int cls) {
        if (a.cls != cls || a.norm.area() <= 0 || b.area() <= 0) return 1e4;
        const double ratio = std::max({static_cast<double>(a.norm.width/b.width),
                                       static_cast<double>(b.width/a.norm.width),
                                       static_cast<double>(a.norm.height/b.height),
                                       static_cast<double>(b.height/a.norm.height)});
        if (ratio > 4.0) return 1e4;
        const cv::Point2f ca(a.norm.x+a.norm.width/2, a.norm.y+a.norm.height/2);
        const cv::Point2f cb(b.x+b.width/2, b.y+b.height/2);
        const double distance = cv::norm(ca-cb);
        // Tighter gating prevents a small defect ID from hopping between
        // two nearby details on the label. Allow some jitter in YOLO boxes.
        const double gate = std::max(0.025, std::min(0.060,
                            2.0*std::max({static_cast<double>(a.norm.width),
                                          static_cast<double>(a.norm.height),
                                          static_cast<double>(b.width),
                                          static_cast<double>(b.height)})));
        if (distance > gate) return 1e4;
        return 0.65*(distance/gate) + 0.35*(1.0-boxIoU(a.norm,b));
    }
    static int popcount(unsigned int bits) {
        int n = 0;
        while (bits) { bits &= bits-1; ++n; }
        return n;
    }
public:
    void reset() {
        std::vector<Item>().swap(items_);
        nextId_ = 1;
    } // Never carry ERR IDs/capacity to another decal.
    struct Confirmed {
        cv::Rect box;
        int cls = 0;
        float confidence = 0;
        size_t id = 0;
        bool held = false;
    };
    TemporalErrorTracker(int window, int minHits, int maxMissed,
                         float strongThreshold, float weakStableThreshold)
        : window_(std::max(1,std::min(8,window))),
          minHits_(std::max(1,std::min(window_,minHits))),
          maxMissed_(std::max(0,maxMissed)),
          strongThreshold_(strongThreshold),
          weakStableThreshold_(weakStableThreshold) {}

    std::vector<Confirmed> update(const std::vector<local_track::Object>& raw,
                                  const cv::Rect& roi, const cv::Size& imageSize) {
        const unsigned int mask = (1u << window_) - 1u;
        std::vector<cv::Rect2f> norm;
        norm.reserve(raw.size());
        for (const auto& d : raw) norm.push_back(normalize(d, roi));
        for (auto& t : items_) {
            t.history = (t.history << 1) & mask;
            t.strongHistory = (t.strongHistory << 1) & mask;
            ++t.missed;
        }
        std::vector<bool> matched(raw.size(), false);
        if (!items_.empty()) {
            const size_t n = items_.size();
            std::vector<std::vector<double>> costs(n, std::vector<double>(raw.size()+n, 0.98));
            for (size_t i=0; i<n; ++i)
                for (size_t j=0; j<raw.size(); ++j)
                    costs[i][j] = matchCost(items_[i], norm[j], raw[j].label);
            auto assignment = local_track::assignMinimum(costs);
            for (size_t i=0; i<n; ++i) {
                const int j = assignment[i];
                if (j < 0 || static_cast<size_t>(j) >= raw.size() ||
                    costs[i][j] >= 0.98) continue;
                Item& t = items_[i];
                // Smooth box jitter while following each distinct error.
                const float oldWeight = 0.4f;
                t.norm.x = oldWeight*t.norm.x + (1-oldWeight)*norm[j].x;
                t.norm.y = oldWeight*t.norm.y + (1-oldWeight)*norm[j].y;
                t.norm.width = oldWeight*t.norm.width + (1-oldWeight)*norm[j].width;
                t.norm.height = oldWeight*t.norm.height + (1-oldWeight)*norm[j].height;
                t.confidence = 0.4f*t.confidence + 0.6f*raw[j].prob;
                t.history |= 1u;
                if (raw[j].prob >= strongThreshold_) t.strongHistory |= 1u;
                t.missed = 0;
                matched[j] = true;
            }
        }
        for (size_t j=0; j<raw.size(); ++j) {
            if (matched[j]) continue;
            Item t;
            t.id=nextId_++;
            t.cls=raw[j].label;
            t.norm=norm[j];
            t.confidence=raw[j].prob;
            t.history=1;
            t.strongHistory = raw[j].prob >= strongThreshold_ ? 1u : 0u;
            t.missed=0;
            items_.push_back(t);
        }
        items_.erase(std::remove_if(items_.begin(),items_.end(),[&](const Item& t){
            return t.missed > maxMissed_ || (!t.latched && popcount(t.history)==0);
        }), items_.end());
        std::vector<Confirmed> out;
        const cv::Rect limits(0,0,imageSize.width,imageSize.height);
        for (auto& t : items_) {
            const int observations = popcount(t.history);
            const int strong = popcount(t.strongHistory);
            // Hysteresis: only use 3/5 or 4/5 for INITIAL confirmation.
            // Once confirmed, don't reclassify the same error every AI frame.
            // Re-show for up to errorMaxMissed missing AI frames; HOLD is marked.
            if (!t.latched) {
                const bool stable = observations >= minHits_;
                const bool reliable = strong >= 2;
            
                if (stable && reliable) {
                    t.latched = true;
                }
            }
            if (!t.latched) continue;
            const cv::Rect r(cvRound(roi.x + t.norm.x*roi.width),
                             cvRound(roi.y + t.norm.y*roi.height),
                             std::max(1,cvRound(t.norm.width*roi.width)),
                             std::max(1,cvRound(t.norm.height*roi.height)));
            const cv::Rect clipped = r & limits;
            if (!clipped.empty())
                out.push_back({clipped,t.cls,t.confidence,t.id,t.missed>0});
        }
        return out;
    }
};
