#pragma once
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "winhttp.lib")

class HttpClient {
public:
    static std::string post(const std::string& host,
        const std::string& path,
        const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& body,
        int timeoutSeconds = 30) {

        std::wstring whost = toWide(host);
        std::wstring wpath = toWide(path);

        HINTERNET hSession = WinHttpOpen(
            L"Groq-Native/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );

        if (!hSession) {
            return "error";
            //throw std::runtime_error("WinHttpOpen failed, code: " + std::to_string(GetLastError()));
        }

        HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(),
            INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            return "error";
            //throw std::runtime_error("WinHttpConnect failed, code: " + std::to_string(GetLastError()));
        }

        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect,
            L"POST",
            wpath.c_str(),
            NULL,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE
        );

        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return "error";
            //throw std::runtime_error("WinHttpOpenRequest failed, code: " + std::to_string(GetLastError()));
        }

        int timeout = timeoutSeconds * 1000;
        WinHttpSetTimeouts(hRequest, timeout, timeout, timeout, timeout);

        std::wstring headerBlock;
        for (const auto& h : headers) {
            headerBlock += toWide(h.first) + L": " + toWide(h.second) + L"\r\n";
        }

        if (!headerBlock.empty()) {
            WinHttpAddRequestHeaders(hRequest, headerBlock.c_str(), -1L,
                WINHTTP_ADDREQ_FLAG_ADD);
        }

        BOOL result = WinHttpSendRequest(
            hRequest,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            (LPVOID)body.c_str(),
            (DWORD)body.length(),
            (DWORD)body.length(),
            0
        );

        if (!result) {
            DWORD err = GetLastError();
            cleanup(hRequest, hConnect, hSession);
            return "error";
            //throw std::runtime_error("WinHttpSendRequest failed, code: " + std::to_string(err));
        }

        result = WinHttpReceiveResponse(hRequest, NULL);
        if (!result) {
            DWORD err = GetLastError();
            cleanup(hRequest, hConnect, hSession);
            return "error";
            //throw std::runtime_error("WinHttpReceiveResponse failed, code: " + std::to_string(err));
        }

        DWORD statusCode = 0;
        DWORD size = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode, &size, WINHTTP_NO_HEADER_INDEX);

        std::string response;
        do {
            DWORD available = 0;
            WinHttpQueryDataAvailable(hRequest, &available);
            if (available == 0) break;

            std::vector<char> buffer(available + 1, 0);
            DWORD downloaded = 0;
            WinHttpReadData(hRequest, buffer.data(), available, &downloaded);
            response.append(buffer.data(), downloaded);
        } while (true);

        cleanup(hRequest, hConnect, hSession);

        if (statusCode != 200) {
            return "error";
            //throw std::runtime_error("HTTP " + std::to_string(statusCode) + ": " + response);
        }

        return response;
    }

    static void cleanup(HINTERNET hRequest, HINTERNET hConnect, HINTERNET hSession) {
        if (hRequest) WinHttpCloseHandle(hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        if (hSession) WinHttpCloseHandle(hSession);
    }

    static std::wstring toWide(const std::string& str) {
        if (str.empty()) return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
        std::wstring wstr(size - 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size);
        return wstr;
    }
};