\# WEB – Web Application



\## 1. Overview | Tổng quan



\*\*Tiếng Việt\*\*



Thành phần WEB cung cấp giao diện giám sát hệ thống thông qua trình duyệt web. Source code hiện tại được xây dựng bằng HTML và tích hợp Firebase để hỗ trợ các chức năng lưu trữ, đồng bộ và quản lý dữ liệu.



\*\*English\*\*



The WEB module provides a browser-based interface for system monitoring. It is implemented using HTML and integrates Firebase for data storage, synchronization, and data management.



\## 2. Technologies | Công nghệ sử dụng



\- HTML

\- JavaScript (if included in the HTML source)

\- GitHub

\- GitHub Pages (if used for hosting)

\- Firebase



\## 3. Deployment | Triển khai



\### Step 1 – Create a GitHub Repository



1\. Create a new repository on GitHub.

2\. Upload the WEB source code.

3\. Ensure the main HTML file is named `index.html`.



\### Step 2 – Configure GitHub Pages



1\. Open the repository on GitHub.

2\. Navigate to \*\*Settings\*\*.

3\. Select \*\*Pages\*\*.

4\. Under Build and deployment, select the appropriate deployment source.

5\. Select the branch containing `index.html`.

6\. Save the configuration.

7\. Access the website using the generated GitHub Pages URL.



\### Step 3 – Configure Firebase



1\. Access the Firebase Console: https://console.firebase.google.com/

2\. Create a Firebase project.

3\. Register a Web App in the project.

4\. Obtain the Firebase Web configuration.

5\. Configure the required Firebase services.

6\. Add the Firebase configuration to the WEB source code.

7\. Configure database access rules and authentication according to the application requirements.



\### Step 4 – Configure GitHub Secrets (If Required)



If the deployment workflow uses GitHub Actions:



1\. Open repository Settings.

2\. Navigate to \*\*Secrets and variables → Actions\*\*.

3\. Create the required repository secrets.

4\. Reference those secrets in the deployment workflow.



\*\*Important Security Notes\*\*



\- Never upload Firebase service account private keys or other privileged credentials.

\- Firebase Web API configuration is not a substitute for database security rules.

\- Configure Firebase Authentication and database rules to restrict unauthorized access.

\- Do not expose administrator credentials in public HTML or JavaScript files.



\## 4. Accessing the Website | Truy cập website



After successful deployment, access the application using the GitHub Pages URL.



Sau khi triển khai thành công, truy cập giao diện thông qua đường dẫn GitHub Pages được cung cấp trong repository settings.



\## 5. Notes | Lưu ý



\- GitHub Pages hosts static web content.

\- Firebase services must be configured separately.

\- Changes pushed to the configured deployment branch will trigger an update according to the deployment configuration.

