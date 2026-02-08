#pragma once
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "winhttp.lib")

class Json {
public:
    static std::string buildChatRequest(const std::string& model,
        const std::vector<std::pair<std::string, std::string>>& messages) {
        std::ostringstream json;
        json << "{";
        json << "\"model\":\"" << escapeString(model) << "\",";
        json << "\"messages\":[";

        for (size_t i = 0; i < messages.size(); ++i) {
            if (i > 0) json << ",";
            json << "{";
            json << "\"role\":\"" << escapeString(messages[i].first) << "\",";
            json << "\"content\":\"" << escapeString(messages[i].second) << "\"";
            json << "}";
        }

        json << "],";
        json << "\"temperature\":0.7,";
        json << "\"max_tokens\":1000";
        json << "}";

        return json.str();
    }

    static std::string extractContent(const std::string& json) {
        std::string errorKey = "\"error\":";
        if (json.find(errorKey) != std::string::npos) {
            return "API Error: " + extractErrorMessage(json);
        }
        std::string key = "\"content\":\"";
        size_t pos = json.find(key);
        if (pos == std::string::npos) {
            key = "\"text\":\"";
            pos = json.find(key);
            if (pos == std::string::npos) {
                return "Parse Error: content field not found";
            }
        }

        pos += key.length();

        std::string result;
        for (size_t i = pos; i < json.length(); ++i) {
            char c = json[i];
            if (c == '\\' && i + 1 < json.length()) {
                char next = json[i + 1];
                switch (next) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                default: result += next; break;
                }
                ++i;
            }
            else if (c == '"') {
                break;
            }
            else {
                result += c;
            }
        }

        return result;
    }

    static std::string extractErrorMessage(const std::string& json) {
        std::string key = "\"message\":\"";
        size_t pos = json.find(key);
        if (pos == std::string::npos) {
            key = "\"error\":\"";
            pos = json.find(key);
            if (pos == std::string::npos) return json;
        }

        pos += key.length();
        std::string result;
        for (size_t i = pos; i < json.length() && json[i] != '"'; ++i) {
            if (json[i] == '\\' && i + 1 < json.length()) {
                result += json[++i];
            }
            else {
                result += json[i];
            }
        }
        return result;
    }

    static std::string escapeString(const std::string& str) {
        std::ostringstream escaped;
        for (char c : str) {
            switch (c) {
            case '"': escaped << "\\\""; break;
            case '\\': escaped << "\\\\"; break;
            case '\n': escaped << "\\n"; break;
            case '\r': escaped << "\\r"; break;
            case '\t': escaped << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    escaped << "\\u" << std::hex << std::setw(4)
                        << std::setfill('0') << static_cast<int>(c);
                }
                else {
                    escaped << c;
                }
            }
        }
        return escaped.str();
    }
};