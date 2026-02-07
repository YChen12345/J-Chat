#define _USE_MATH_DEFINES
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <set>
#include <queue>
#include <condition_variable>
#include <map>
#include <algorithm>

#include <iostream>
#include <fmod.hpp>
#include <fmod_errors.h>
#include <cmath>
#include <conio.h>

#pragma comment(lib, "fmod_vc.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "d3d11.lib")

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#define DEFAULT_PORT "8080"
#define MAX_MESSAGES 200
#define MAX_CLIENTS 10

//-----------------------------------------
//Client type
enum class ClientType {
    NONE,
    SERVER,
    CLIENT
};
//------------------------------------------
//Message data structure
struct ChatMessage {
    std::string sender;
    std::string content;
    std::string timestamp;
    bool isSelf;
    bool isSystem;//System automatic message
    bool isPrivate;//Is a private message
    std::string recipient; //Private message recipient
    int recipientID; //Private message recipient id

    ChatMessage() : isSelf(false), isSystem(false), isPrivate(false) {}
    ChatMessage(const std::string& s, const std::string& c, const std::string& t, bool self, bool sys, bool priv = false, const std::string& r = "", const int targetID = 0)
        : sender(s), content(c), timestamp(t), isSelf(self), isSystem(sys), isPrivate(priv), recipient(r), recipientID(targetID) {
    }
};
//----------------------------------------------------------------------------------------------------------------------------------------------------
//Private chat window
struct PrivateChatWindow {
    std::string targetUser;//Target user name
    std::string targetId;//Target user ID
    bool isOpen;//Window is open
    std::vector<ChatMessage> messages;
    char inputBuffer[256];
    bool scrollToBottom;
    bool hasNewMessage = {false};

    PrivateChatWindow() : targetUser(""), targetId(""), isOpen(false), scrollToBottom(false) {
        memset(inputBuffer, 0, sizeof(inputBuffer));
    }
    PrivateChatWindow(const std::string& user, const std::string& id)
        : targetUser(user), targetId(id), isOpen(true), scrollToBottom(false) {
        memset(inputBuffer, 0, sizeof(inputBuffer));
    }
};
//---------------------------------------------------------------------------------------------------
//Online user information
struct OnlineUser {
    std::string name;//User name
    std::string id;//User id
    std::string ip;
    bool isSelected;

    OnlineUser() : isSelected(false) {}
    OnlineUser(const std::string& n, const std::string& i, const std::string& ipAddr = "")
        : name(n), id(i), ip(ipAddr), isSelected(false) {
    }
};
//----------------------------------------------------------------------------------------------
//Client connection information
struct ClientConnection {
    SOCKET socket;
    std::string ip;
    std::string name;//Client user nickname
    std::thread thread;
    std::atomic<bool> active{ true };
    int id;

    ClientConnection() : socket(INVALID_SOCKET), id(0) {}
};
//-----------------------------------------------------------------------------------------------
//Network state
struct NetworkState {
    //General
    std::atomic<bool> connected{ false };
    std::atomic<bool> shouldClose{ false };
    std::atomic<bool> isRunning{ false };//Server is running
    std::string status = "Not connected";
    std::string localIP;
    ClientType type = ClientType::NONE;
    int myid=0;

    //Server attributes
    SOCKET listenSocket = INVALID_SOCKET;
    std::vector<std::unique_ptr<ClientConnection>> clients;
    std::mutex clientsMutex;
    int nextClientId = 1;

    //Client attributes
    SOCKET clientSocket = INVALID_SOCKET;
    std::string serverIP;
};

//Broadcast message queue
struct BroadcastMessage {
    std::string content;
    int senderId;//Sender id (0 indicates a server system message)   
    std::string senderName;
    bool isSystem;//System automatic message
    bool isPrivate;//Is a private message
    int targetId;//Private chat target id (-1 means broadcast to all, 0 means broadcast to server only)

    BroadcastMessage() : senderId(0), isPrivate(false), isSystem(false), targetId(-1) {}
    BroadcastMessage(const std::string& c, int sid, const std::string& sname, bool isSys = false, bool priv = false, int tid = -1)
        : content(c), senderId(sid), senderName(sname), isPrivate(priv), isSystem(isSys), targetId(tid) {
    }
};

//------------------------------------------------------------------------

static NetworkState g_network;
static std::vector<ChatMessage> g_messages;//Record chat message
static std::mutex g_msgMutex;
static char g_inputBuffer[256] = "";
static char g_serverIP[256] = "127.0.0.1";
static char g_nickname[64] = "User";
static bool g_scrollToBottom = false;

//User list and private chat
static std::vector<OnlineUser> g_onlineUsers;
static std::mutex g_usersMutex;
static std::map<int, PrivateChatWindow> g_privateChats; //Key: (int)id
static std::mutex g_privateChatsMutex;
static std::mutex g_privateChatsWindowMutex;
static std::string g_selectedUserId; //Currently selected user ID

//Broadcast queue
static std::queue<BroadcastMessage> g_broadcastQueue;
static std::mutex g_broadcastMutex;
static std::condition_variable g_broadcastCV;

//DirectX11
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static bool starting = { false };
static std::atomic<bool> systemRunning = { true };
static bool soundEffect = { true };
static bool hasBGM = { true };

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();

//--------------------------------------------------------------------------------------------------
//Fmod
FMOD_RESULT F_CALLBACK sineCallback(FMOD_DSP_STATE* dsp_state, float* inbuffer, float* outbuffer, unsigned int length, int
    inchannels, int* outchannels)
{
    static float phase = 0.0f;
    const float frequency = 440.0f;
    const float sampleRate = 48000.0f;
    for (unsigned int i = 0; i < length; i++) {
        float sample = sinf(phase);
        phase += 2.0f * M_PI * frequency / sampleRate;
        if (phase >= 2.0f * M_PI) {
            phase -= 2.0f * M_PI;
        }
        for (int j = 0; j < *outchannels; j++) {
            outbuffer[i * (*outchannels) + j] = sample;
        }
    }
    return FMOD_OK;
}
void sineWave() {
    if (!soundEffect) {
        return;
    }
    FMOD::System* system;
    FMOD::System_Create(&system);
    system->init(128, FMOD_INIT_NORMAL, NULL);
    FMOD_DSP_DESCRIPTION dspDesc = {};
    dspDesc.version = 0x00010000;
    dspDesc.numinputbuffers = 0;
    dspDesc.numoutputbuffers = 1;
    dspDesc.read = sineCallback;
    FMOD::DSP* dsp;
    system->createDSP(&dspDesc, &dsp);
    system->playDSP(dsp, NULL, false, NULL);
    auto start = std::chrono::steady_clock::now();
    while (true) 
    {
        if (!systemRunning) {
            break;
        }
        system->update();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        if (elapsed >= 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    dsp->release();
    system->close();
    system->release();
}
void playMusic(std::string musicName)
{
    if (!soundEffect) {
        return;
    }
    FMOD::System* system;
    FMOD::System_Create(&system);
    system->init(128, FMOD_INIT_NORMAL, NULL);
    FMOD::Sound* sound = NULL;
    FMOD::Channel* channel = NULL;
    system->createSound((musicName + ".mp3").c_str(), FMOD_DEFAULT, NULL, &sound);
    system->playSound(sound, NULL, false, &channel);
    auto start = std::chrono::steady_clock::now();
    while (true)
    {
        if (!systemRunning) {
            break;
        }
        system->update();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        if (elapsed >= 3) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    sound->release();
    system->close();
    system->release();
}
void playBGM(std::string musicName)
{
    FMOD::System* system;
    FMOD::System_Create(&system);
    system->init(128, FMOD_INIT_NORMAL, NULL);
    FMOD::Sound* sound = NULL;
    FMOD::Channel* channel = NULL;
    system->createSound((musicName + ".mp3").c_str(), FMOD_LOOP_NORMAL, NULL, &sound);
    system->playSound(sound, NULL, false, &channel);
    auto start = std::chrono::steady_clock::now();
    while (true)
    {
        if (hasBGM) {
            channel->setPaused(false);
        }
        else
        {
            channel->setPaused(true);
        }
        if (!systemRunning) {
            break;
        }
    }
    sound->release();
    system->close();
    system->release();
}

//------------------------------------------------------------------------

std::string GetCurrentTimeStr() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time);
    std::stringstream ss;
    ss << std::put_time(&tm, "%H:%M:%S");
    return ss.str();
}

void PrintAddressInfo(const sockaddr* addr, std::string& out) {
    char ipStr[INET6_ADDRSTRLEN];
    int port = 0;
    if (addr->sa_family == AF_INET) {
        sockaddr_in* sin = (sockaddr_in*)addr;
        inet_ntop(AF_INET, &sin->sin_addr, ipStr, sizeof(ipStr));
        port = ntohs(sin->sin_port);
        out = std::string(ipStr) + ":" + std::to_string(port);
    }
    else {
        sockaddr_in6* sin6 = (sockaddr_in6*)addr;
        inet_ntop(AF_INET6, &sin6->sin6_addr, ipStr, sizeof(ipStr));
        port = ntohs(sin6->sin6_port);
        out = "[" + std::string(ipStr) + "]:" + std::to_string(port);
    }
}

//Add room chat message
void AddMessage(const std::string& sender, const std::string& content, bool isSelf, bool isSystem = false, bool isPrivate = false, const std::string& recipient = "", const int recipientID = 0) {
    std::lock_guard<std::mutex> lock(g_msgMutex);
    if (g_messages.size() >= MAX_MESSAGES) {
        g_messages.erase(g_messages.begin());
    }
    g_messages.push_back(ChatMessage(sender, content, GetCurrentTimeStr(), isSelf, isSystem, isPrivate, recipient, recipientID));
    if (!isSelf&&!isSystem) {
        std::thread(sineWave).detach();
    }
    if (!isPrivate) g_scrollToBottom = true;
}

//Add private chat message
void AddPrivateMessage(const int windowKey, const std::string& sender, const std::string& content, bool isSelf) {
    std::lock_guard<std::mutex> lock(g_privateChatsMutex);
    auto it = g_privateChats.find(windowKey);
    if (it != g_privateChats.end()) {
        if (it->second.messages.size() >= MAX_MESSAGES) {
            it->second.messages.erase(it->second.messages.begin());
        }
        it->second.messages.push_back(ChatMessage(sender, content, GetCurrentTimeStr(), isSelf, false, true, it->second.targetUser,std::stoi(it->second.targetId)));
        it->second.scrollToBottom = true;
        if (!isSelf) {
            it->second.hasNewMessage = true;
            std::thread(playMusic, "Sounds/mg").detach();
        }
    }
}

//Get or create the private chat window
PrivateChatWindow* GetOrCreatePrivateChat(const std::string& userName, const std::string& userId) {
    int key = std::stoi(userId);
    std::lock_guard<std::mutex> lock(g_privateChatsMutex);
    auto it = g_privateChats.find(key);
    if (it == g_privateChats.end()) {
        auto result =
            g_privateChats.emplace(key, PrivateChatWindow(userName, userId));
        if (result.second) {
            result.first->second.isOpen = true;
            return &(result.first->second);
        }
        return NULL;
    }
    it->second.isOpen = true;
    return &(it->second);
}

//----------------------------------------------------------------------------------------------------------------
bool SendMsg(SOCKET sock, const std::string& content) {
    uint32_t len = htonl(content.size());
    send(sock, (char*)&len, 4, 0);
    return send(sock, content.c_str(), content.size(), 0) != SOCKET_ERROR;;
}

int RecvMsg(SOCKET sock, std::string& out) {
    uint32_t lenNet;
    if (!recv(sock, (char*)&lenNet, 4,0)) return false;

    uint32_t len = ntohl(lenNet);
    out.resize(len);
    return recv(sock, out.data(), len,0);
}
//Server broadcast the message to all clients
void BroadcastMessageToAll(const std::string& content, int senderId, const std::string& senderName, bool isSystem = false, bool isPrivate = false, int targetId = -1) {
    std::lock_guard<std::mutex> lock(g_broadcastMutex);
    g_broadcastQueue.push(BroadcastMessage(content, senderId, senderName, isSystem, isPrivate, targetId));
    g_broadcastCV.notify_one();
}
void UpdateServerOnlineUserList() {
    g_onlineUsers.push_back(OnlineUser(g_nickname, "0", g_serverIP));
    for (int i = 0; i < g_network.clients.size(); i++) {
        std::unique_ptr<ClientConnection>& client = g_network.clients[i];
        if (client && client->active) {
            g_onlineUsers.push_back(OnlineUser(client->name, std::to_string(client->id), client->ip));
        }
    }
}
void BroadcastUserList() {
    if (g_network.type != ClientType::SERVER || !g_network.isRunning) return;

    std::string userListMsg = "!users ";
    bool first = true;

    std::lock_guard<std::mutex> lock(g_network.clientsMutex);
    userListMsg += g_nickname;
    userListMsg += "#" + std::to_string(0) + "#" + g_serverIP;
    first = false;
    for (int i = 0; i < g_network.clients.size(); i++) {
        std::unique_ptr<ClientConnection>& client = g_network.clients[i];
        if (client && client->active) {
            if (!first) userListMsg += ",";
            userListMsg += client->name + "#" + std::to_string(client->id) + "#" + client->ip;
        }
    }
    UpdateServerOnlineUserList();
    BroadcastMessageToAll(userListMsg, 0, "System", true);
}

//Broadcast worker thread
void BroadcastWorker() {
    while (!g_network.shouldClose) {
        std::unique_lock<std::mutex> lock(g_broadcastMutex);
        g_broadcastCV.wait(lock, [] { return !g_broadcastQueue.empty() || g_network.shouldClose; });

        if (g_network.shouldClose) break;

        while (!g_broadcastQueue.empty()) {
            BroadcastMessage msg = g_broadcastQueue.front();
            g_broadcastQueue.pop();
            lock.unlock();

            //Private message format:  @[TargetID][Server/Client][SenderID]SenderName: Content
            //System message format: [System]Content
            //Server message format: [Server]Content
            //Other message format: [Client][Uid]Uname: Content
            std::string formattedMsg;
            if (msg.isPrivate) {
                if (msg.senderId == 0) {
                    formattedMsg = "@[" + std::to_string(msg.targetId) + "]" +
                        "[Server][" + std::to_string(msg.senderId) + "]" + msg.senderName + ": " + msg.content;
                }
                else
                {
                    formattedMsg = "@[" + std::to_string(msg.targetId) + "]" +
                        "[Client][" + std::to_string(msg.senderId) + "]" + msg.senderName + ": " + msg.content;
                }             
            }
            else if (msg.isSystem) {
                formattedMsg = "[System]" + msg.content;
            }
            else if (msg.senderId == 0) {
                formattedMsg = "[Server][" + std::to_string(msg.senderId) + "]" + msg.senderName + ": " + msg.content;
            }
            else {
                formattedMsg = "[Client][" + std::to_string(msg.senderId) + "]" + msg.senderName + ": " + msg.content;
            }
            //Send to target user
            std::lock_guard<std::mutex> clientLock(g_network.clientsMutex);
            for (int i = 0; i < g_network.clients.size(); i++) {
                std::unique_ptr<ClientConnection>& client = g_network.clients[i];
                if (client && client->active && client->socket != INVALID_SOCKET) {
                    if (msg.isPrivate) {
                        if (client->id == msg.targetId) {
                            SendMsg(client->socket, formattedMsg);
                            continue;
                        }
                    }
                    else if (msg.isSystem) {
                        SendMsg(client->socket, formattedMsg);
                    }
                    else
                    {
                        if (client->id != msg.senderId) {
                            SendMsg(client->socket, formattedMsg);
                        }
                    }
                }
            }
            lock.lock();
        }
    }
}

//--------------------------------------------------------------------------------------------------------
void SetNickname(const std::string& name) {
    int len = name.length();
    if (len >= sizeof(g_nickname)) {
        len = sizeof(g_nickname) - 1;
    }
    memcpy(g_nickname, name.c_str(), len);
    g_nickname[len] = '\0';
}

void HandleNameChangeCommand(const std::string& newName) {
    std::string oldName = g_nickname;
    SetNickname(newName);

    if (g_network.type == ClientType::SERVER && g_network.isRunning) {
        BroadcastMessageToAll("Server changed name to " + newName, 0, "System", true);
        AddMessage("Server", oldName + " -> " + newName, false, true);
        BroadcastUserList();
    }
    else if (g_network.type == ClientType::CLIENT && g_network.connected) {
        std::string nameCmd = "!name " + newName;
        SendMsg(g_network.clientSocket, nameCmd);
        AddMessage("System", "Nickname changed to: " + newName, false, true);
    }
    else {
        AddMessage("System", "Nickname changed to: " + newName, false, true);
    }
}

void HandleClient(ClientConnection* client) {
    std::string buffer;
    client->active = true;

    std::string welcome = "[System]Welcome! Your ID is [" + std::to_string(client->id) + "]\n";
    //BroadcastMessageToAll(welcome, 0, "System", true, true, client->id);
    SendMsg(client->socket, welcome);

    BroadcastMessageToAll("User [" + std::to_string(client->id) + "] joined the chat.\n", 0, "System", true);
    AddMessage("System", "User [" + std::to_string(client->id) + "] (" + client->ip + ") joined", false, true);

    BroadcastUserList();

    while (!g_network.shouldClose && client->active) {
        int bytes = RecvMsg(client->socket, buffer);
        if (bytes>0&&buffer.size()>0) {
            std::string msg(buffer);

            if (msg.substr(0, 1) == "@") {
                int closeBracket = msg.find(']');
                if (closeBracket != std::string::npos && closeBracket > 2) {
                    std::string targetIdStr = msg.substr(2, closeBracket - 2);
                    int targetId = std::stoi(targetIdStr);
                    std::string content = msg.substr(closeBracket + 1);                  

                    if (targetId == 0) {
                        PrivateChatWindow* chat = GetOrCreatePrivateChat(client->name, std::to_string(client->id));
                        AddPrivateMessage(client->id,client->name,content,false);
                    }
                    else
                    {
                        BroadcastMessageToAll(content, client->id, client->name, false, true, targetId);
                    }
                    continue;
                }
            }

            if (msg.substr(0, 6) == "!name ") {
                std::string oldName = client->name;
                std::string newName = msg.substr(6);
                client->name = newName;

                BroadcastMessageToAll("User[" + std::to_string(client->id) + "]changed name to " + client->name, 0, "System",true);
                AddMessage("System", "[" + std::to_string(client->id) + "] " + oldName + " -> " + client->name, false, true);

                BroadcastUserList();
                continue;
            }

            if (msg == "!bye" || msg == "!quit") {
                break;
            }

            if (msg == "!list") {
                std::string list = "[System]Online users:\n";
                std::lock_guard<std::mutex> lock(g_network.clientsMutex);
                for (int i = 0; i < g_network.clients.size(); i++) {
                    std::unique_ptr<ClientConnection>& c = g_network.clients[i];
                    if (c && c->active) {
                        list += "[" + std::to_string(c->id) + "]" + c->name + " (" + c->ip + ")\n";
                    }
                }
                SendMsg(client->socket, list);
                continue;
            }

            AddMessage("[Client][" + std::to_string(client->id) + "]" + client->name, msg, false);
            BroadcastMessageToAll(msg, client->id, client->name);

        }
        else if (bytes == 0 || bytes == SOCKET_ERROR) {
            break;
        }
    }

    //Client disconnected
    client->active = false;
    BroadcastMessageToAll("User [" + std::to_string(client->id) + "] (" + client->name + ") left", 0, "System",true);
    AddMessage("System", "User [" + std::to_string(client->id) + "] (" + client->ip + ") disconnected", false, true);

    BroadcastUserList();

    if (client->socket != INVALID_SOCKET) {
        closesocket(client->socket);
        client->socket = INVALID_SOCKET;
    }
}

void CleanupClients() {
    std::lock_guard<std::mutex> lock(g_network.clientsMutex);
    auto it = g_network.clients.begin();
    while (it != g_network.clients.end()) {
        if (!(*it)->active) {
            if ((*it)->thread.joinable()) {
                (*it)->thread.join();
            }
            it = g_network.clients.erase(it);
        }
        else {
            it++;
        }
    }
}

//--------------------------------------------------------------------------------------------------
//Start server
void AcceptLoop() {
    while (!g_network.shouldClose && g_network.isRunning) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(g_network.listenSocket, &readSet);

        timeval timeout = { 3, 0 };
        int result = select(0, &readSet, nullptr, nullptr, &timeout);

        if (result > 0 && FD_ISSET(g_network.listenSocket, &readSet)) {
            sockaddr_storage clientAddr;
            int addrLen = sizeof(clientAddr);
            SOCKET clientSocket = accept(g_network.listenSocket, (sockaddr*)&clientAddr, &addrLen);

            if (clientSocket != INVALID_SOCKET) {
                {
                    std::lock_guard<std::mutex> lock(g_network.clientsMutex);
                    if (g_network.clients.size() >= MAX_CLIENTS) {
                        std::string fullMsg = "[System]Server is full. Try again later.";
                        SendMsg(clientSocket, fullMsg);
                        closesocket(clientSocket);
                        AddMessage("System", "Rejected connection: server full", false, true);
                        continue;
                    }
                }

                std::unique_ptr<ClientConnection> client(new ClientConnection());
                client->socket = clientSocket;
                client->id = g_network.nextClientId++;
                client->name = "User" + std::to_string(client->id);
                PrintAddressInfo((sockaddr*)&clientAddr, client->ip);

                ClientConnection* clientPtr = client.get();
                {
                    std::lock_guard<std::mutex> lock(g_network.clientsMutex);
                    g_network.clients.push_back(std::move(client));
                }

                //Start the client processing thread
                g_network.clients.back()->thread = std::thread([clientPtr]() {HandleClient(clientPtr);});
            }
        }

        //Regularly clear disconnected clients
        static auto lastCleanup = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCleanup).count() > 5) {
            CleanupClients();
            lastCleanup = now;
        }
    }
}

void StartServer() {
    std::thread([]() {
        g_network.listenSocket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (g_network.listenSocket == INVALID_SOCKET) {
            AddMessage("System", "Failed to create socket", false, true);
            return;
        }

        //Support both Ipv4 and Ipv6
        int v6only = 0;
        setsockopt(g_network.listenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&v6only, sizeof(v6only));

        int reuse = 1;
        setsockopt(g_network.listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

        sockaddr_in6 addr6 = {};
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(8080);
        addr6.sin6_addr = in6addr_any;

        if (bind(g_network.listenSocket, (sockaddr*)&addr6, sizeof(addr6)) == SOCKET_ERROR) {
            AddMessage("System", "Binding failed, port may be in use", false, true);
            closesocket(g_network.listenSocket);
            g_network.listenSocket = INVALID_SOCKET;
            return;
        }

        listen(g_network.listenSocket, SOMAXCONN);
        g_network.localIP = "[::]:8080";
        g_network.status = "Running (0 clients)";
        g_network.shouldClose = false;
        g_network.connected = true;
        g_network.isRunning = true;
        g_network.type = ClientType::SERVER;

        AddMessage("System", "Server started on port 8080", false, true);
        AddMessage("System", "Waiting for clients...", false, true);

        std::thread broadcastThread(BroadcastWorker);
        UpdateServerOnlineUserList();
        AcceptLoop();

        g_network.shouldClose = true;
        g_broadcastCV.notify_all();

        if (broadcastThread.joinable()) {
            broadcastThread.join();
        }

        //Close all client connections
        {
            std::lock_guard<std::mutex> lock(g_network.clientsMutex);
            for (int i = 0; i < g_network.clients.size(); i++) {
                std::unique_ptr<ClientConnection>& client = g_network.clients[i];
                if (client && client->active) {
                    client->active = false;
                    if (client->socket != INVALID_SOCKET) {
                        closesocket(client->socket);
                    }
                }
            }
            for (int i = 0; i < g_network.clients.size(); i++) {
                std::unique_ptr<ClientConnection>& client = g_network.clients[i];
                if (client->thread.joinable()) {
                    client->thread.join();
                }
            }
            g_network.clients.clear();
        }

        if (g_network.listenSocket != INVALID_SOCKET) {
            closesocket(g_network.listenSocket);
            g_network.listenSocket = INVALID_SOCKET;
        }

        g_network.connected = false;
        g_network.isRunning = false;
        g_network.status = "Stopped";
        AddMessage("System", "Server stopped", false, true);
        }).detach();
}

//----------------------------------------------------------------------------------------
//Client
bool UpdateOnlineUsersList(const std::string& msg) {
    if (msg.size() > 15) {
        if (msg.substr(0, 15) == "[System]!users ") {
            std::string usersData = msg.substr(15);
            std::lock_guard<std::mutex> lock(g_usersMutex);

            g_onlineUsers.clear();

            int pos = 0;
            while (pos < usersData.length()) {
                int commaPos = usersData.find(',', pos);
                std::string userData;
                if (commaPos == std::string::npos) {
                    userData = usersData.substr(pos);
                    pos = usersData.length();
                }
                else {
                    userData = usersData.substr(pos, commaPos - pos);
                    pos = commaPos + 1;
                }

                int hashPos1 = userData.find('#');
                int hashPos2 = userData.find('#', hashPos1 + 1);
                if (hashPos1 != std::string::npos && hashPos2 != std::string::npos) {
                    std::string name = userData.substr(0, hashPos1);
                    std::string id = userData.substr(hashPos1 + 1, hashPos2 - hashPos1 - 1);
                    std::string ip = userData.substr(hashPos2 + 1);
                    g_onlineUsers.push_back(OnlineUser(name, id, ip));
                }
            }
            return true;
        }
        else
        {
            return false;
        }
    }
    else
    {
        return false;
    }
}

void ReceiveThreadClient() {
    std::string buffer;
    while (!g_network.shouldClose && g_network.connected) {
        int bytes = RecvMsg(g_network.clientSocket, buffer);
        if (bytes > 0&&buffer.size()>0) {
            std::string msg(buffer);
            if (UpdateOnlineUsersList(msg)) {
                continue;
            }
            //Message format: @[TargetID][Server/Client][SenderID]SenderName: Content
            if (msg.substr(0, 1) == "@"&&msg.size()>5) {
                int firstClose = msg.find(']');
                int secondClose = msg.find(']',firstClose+1);
                int thirdClose = msg.find(']', secondClose+1);

                if (firstClose != std::string::npos && secondClose != std::string::npos && thirdClose != std::string::npos) {
                    std::string targetId = msg.substr(2, firstClose - 2);
                    std::string senderId = msg.substr(secondClose + 2, thirdClose - secondClose - 2);
                    int colonPos = msg.find(':', thirdClose);

                    if (colonPos != std::string::npos) {
                        std::string senderName = msg.substr(thirdClose + 1, colonPos - thirdClose - 1);
                        std::string content = msg.substr(colonPos + 2);

                        //Get or create a private chat window
                        int windowKey = std::stoi(senderId);
                        PrivateChatWindow* chat = GetOrCreatePrivateChat(senderName, senderId);
                        bool isSelf = (std::stoi(senderId) == g_network.myid);
                        AddPrivateMessage(windowKey, senderName, content, isSelf);
                        continue;
                    }
                }
            }

            std::string sender;
            std::string content;
            bool isSystem = false;
            if (msg.size() > 16) {
                if (msg.substr(0, 15) == "[System]Welcome") {
                    sender = "System";
                    int secondOpen = msg.find('[', 15);
                    int secondClose = msg.find(']', 15);
                    if (secondOpen != std::string::npos && secondClose != std::string::npos) {
                        g_network.myid = std::stoi(msg.substr(secondOpen + 1, secondClose));                     
                    }       
                    content = msg.substr(8);
                    isSystem = true;
                }
                else if (msg.substr(0, 8) == "[System]") {
                    sender = "System";
                    content = msg.substr(8);
                    isSystem = true;
                }
                else if (msg.substr(0, 8) == "[Server]") {
                    int colonPos = msg.find(':');
                    if (colonPos != std::string::npos) {
                        sender = msg.substr(0, colonPos);
                        content = msg.substr(colonPos + 2);
                    }
                }
                else if (msg.substr(0, 8) == "[Client]") {
                    int colonPos = msg.find(':');
                    if (colonPos != std::string::npos) {
                        sender = msg.substr(8, colonPos-8);
                        content = msg.substr(colonPos + 2);
                    }
                }
            }
            else if (msg.size() > 8) {
                if (msg.substr(0, 8) == "[System]") {
                    sender = "System";
                    content = msg.substr(8);
                    isSystem = true;
                }
                else if (msg.substr(0, 8) == "[Server]") {
                    int colonPos = msg.find(':');
                    if (colonPos != std::string::npos) {
                        sender = msg.substr(0, colonPos);
                        content = msg.substr(colonPos + 2);
                    }
                }
                else if (msg.substr(0, 8) == "[Client]") {
                    int colonPos = msg.find(':');
                    if (colonPos != std::string::npos) {
                        sender = msg.substr(8, colonPos-8);
                        content = msg.substr(colonPos + 2);
                    }
                }
            }            
            else{
                int colonPos = msg.find(':');
                if (colonPos != std::string::npos) {
                    sender = msg.substr(0, colonPos);
                    content = msg.substr(colonPos + 2);
                }
                else {
                    sender = "System";
                    content = msg;
                    isSystem = true;
                }
            }

            AddMessage(sender, content, false, isSystem);

        }
        else if (bytes == 0 || bytes == SOCKET_ERROR) {
            if (!g_network.shouldClose) {
                AddMessage("System", "Connection lost", false, true);
            }
            g_network.connected = false;
            break;
        }
    }

    if (g_network.clientSocket != INVALID_SOCKET) {
        closesocket(g_network.clientSocket);
        g_network.clientSocket = INVALID_SOCKET;
    }
}

void StartClient(const std::string& ip) {
    std::thread([ip]() {
        addrinfo hints = {}, * result = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(ip.c_str(), DEFAULT_PORT, &hints, &result) != 0) {
            AddMessage("System", "Address resolution failed: " + ip, false, true);
            return;
        }

        for (addrinfo* ptr = result; ptr; ptr = ptr->ai_next) {
            g_network.clientSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
            if (g_network.clientSocket == INVALID_SOCKET) continue;

            std::string addrStr;
            PrintAddressInfo(ptr->ai_addr, addrStr);
            g_network.status = "Connecting to " + addrStr + "...";

            if (connect(g_network.clientSocket, ptr->ai_addr, (int)ptr->ai_addrlen) == 0) {
                g_network.serverIP = addrStr;
                g_network.shouldClose = false;
                g_network.connected = true;
                g_network.isRunning = true;
                g_network.type = ClientType::CLIENT;
                g_network.status = "Connected to " + addrStr;
                AddMessage("System", "Connected to " + addrStr, false, true);

                std::string nameCmd = "!name " + std::string(g_nickname);
                SendMsg(g_network.clientSocket, nameCmd);

                freeaddrinfo(result);
                ReceiveThreadClient();
                return;
            }
            closesocket(g_network.clientSocket);
            g_network.clientSocket = INVALID_SOCKET;
        }

        freeaddrinfo(result);
        g_network.status = "Connection failed";
        AddMessage("System", "Failed to connect to: " + ip, false, true);
        starting = false;
        }).detach();
}

//------------------------------------------------------------------------------------------
//Connection manager
void Disconnect() {
    if (g_network.type == ClientType::CLIENT && g_network.connected) {
        std::string exit = "!bye";
        SendMsg(g_network.clientSocket, exit);
    }

    g_network.shouldClose = true;
    g_network.connected = false;
    g_network.isRunning = false;
    g_broadcastCV.notify_all();

    //Client
    if (g_network.clientSocket != INVALID_SOCKET) {
        closesocket(g_network.clientSocket);
        g_network.clientSocket = INVALID_SOCKET;
    }

    //Server
    if (g_network.listenSocket != INVALID_SOCKET) {
        closesocket(g_network.listenSocket);
        g_network.listenSocket = INVALID_SOCKET;
    }

    {
        std::lock_guard<std::mutex> lock(g_usersMutex);
        g_onlineUsers.clear();
    }

    g_network.status = "Disconnected";
    g_network.type = ClientType::NONE;
    AddMessage("System", "Disconnected", false, true);
}

bool SendNetworkMessage(const std::string& content) {
    if (!g_network.connected) return false;

    if (g_network.type == ClientType::CLIENT) {        
        return SendMsg(g_network.clientSocket, content);
    }
    else if (g_network.type == ClientType::SERVER) {
        AddMessage("[Server]" + std::string(g_nickname), content, true);
        BroadcastMessageToAll(content, 0, g_nickname);
        return true;
    }
    return false;
}

bool SendPrivateMessage(const std::string& targetId, const std::string& content) {
    if (!g_network.connected) return false;

    int tid = 0;
    tid = std::stoi(targetId);

    std::string msg = "@[" + std::to_string(tid) + "]" + content;

    if (g_network.type == ClientType::CLIENT) {
        return SendMsg(g_network.clientSocket, msg);
    }
    else if (g_network.type == ClientType::SERVER) {
        if (tid == 0) {
            return true;
        }
        else {
            BroadcastMessageToAll(content, 0, g_nickname, false, true, tid);
            return true;
        }
    }
    return false;
}

//--------------------------------------------------------------------------------------------------
//imGUI

ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

void DrawUserListPanel(float width, float height) {
    ImGui::BeginChild("UserList", ImVec2(width, height), true);
    ImGui::Text("Users:");
    ImGui::Separator();

    std::vector<std::pair<std::string, std::string>> users;

    if (g_network.type == ClientType::SERVER) {
        std::lock_guard<std::mutex> lock(g_network.clientsMutex);
        users.push_back(std::make_pair(std::string(g_nickname), "0"));
        for (const auto& client : g_network.clients) {
            users.push_back(std::make_pair(client->name, std::to_string(client->id)));
        }
    }
    else if (g_network.type == ClientType::CLIENT && g_network.connected) {
        std::lock_guard<std::mutex> lock(g_usersMutex);
        for (const auto& user : g_onlineUsers) {
            users.push_back(std::make_pair(user.name, user.id));
        }
    }

    for (int i = 0; i < users.size(); i++) {
        const std::string& name = users[i].first;
        const std::string& id = users[i].second;
        bool isSelected = (g_selectedUserId == id);
        std::string label = name + " [" + id + "]";

        if (isSelected) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.4f, 0.6f, 1.0f));
        }
        if (ImGui::Selectable(label.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
            g_selectedUserId = id;

            //Double-click to open the private chat window
            if (ImGui::IsMouseDoubleClicked(0)) {
                GetOrCreatePrivateChat(name, id);
            }
        }
        if (isSelected) {
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
}

void DrawPrivateChatWindows() {
    std::lock_guard<std::mutex> lock(g_privateChatsWindowMutex);
    std::vector<int> toRemove;

    for (auto it = g_privateChats.begin();
        it != g_privateChats.end(); it++) {
        const int key = it->first;
        PrivateChatWindow& chat = it->second;

        auto user = std::find_if(g_onlineUsers.begin(), g_onlineUsers.end(),
            [key](const OnlineUser& olu) { return std::stoi(olu.id) == key;});
        if (user == g_onlineUsers.end()) {
            toRemove.push_back(key);
            continue;
        }
        if (!chat.isOpen) {
            continue;
        }

        bool isOpen = chat.isOpen;
        bool hasNewMessage = chat.hasNewMessage;

        ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
        std::string windowTitle = "Private Chat with " + chat.targetUser + " [" + chat.targetId + "]";

        if (hasNewMessage) {
            ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        }

        if (ImGui::Begin(windowTitle.c_str(), &isOpen)) {
            ImGui::BeginChild("PrivateHistory", ImVec2(0, -35), true);
            for (int i = 0; i < chat.messages.size(); i++) {
                const ChatMessage& msg = chat.messages[i];
                ImVec4 color = msg.isSelf ? ImVec4(0.3f, 0.7f, 1.0f, 1.0f) : ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
                ImGui::TextColored(color, "[%s] %s: %s", msg.timestamp.c_str(), msg.sender.c_str(), msg.content.c_str());
            }

            if (chat.scrollToBottom || chat.hasNewMessage) {
                ImGui::SetScrollHereY(1.0f);
                chat.scrollToBottom = false;
                chat.hasNewMessage = false;
            }
            ImGui::EndChild();

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 4));
            float inputWidth = ImGui::GetContentRegionAvail().x - 80;
            ImGui::SetNextItemWidth(inputWidth);

            std::string inputLabel = std::string("##private_input") + std::to_string(key);
            bool enterPressed = ImGui::InputText(inputLabel.c_str(), chat.inputBuffer, sizeof(chat.inputBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();

            std::string sendLabel = std::string("Send##") + std::to_string(key);
            bool btnClicked = ImGui::Button(sendLabel.c_str(), ImVec2(75, 0));

            if ((enterPressed || btnClicked) && strlen(chat.inputBuffer) > 0) {
                std::string content(chat.inputBuffer);
                if (std::stoi(chat.targetId)!= g_network.myid) {
                    if (SendPrivateMessage(chat.targetId, content)) {
                        AddPrivateMessage(key, "[Me]" + std::string(g_nickname), content, true);
                    }
                }
                else
                {
                    AddPrivateMessage(key, "[Me]" + std::string(g_nickname), content, true);
                }
                chat.inputBuffer[0] = '\0';
                ImGui::SetKeyboardFocusHere(-1);
            }
            ImGui::PopStyleVar();
        }
        ImGui::End();

        if (hasNewMessage) {
            ImGui::PopStyleColor(3);
        }

        chat.isOpen = isOpen;
    }

    for (int i = 0; i < toRemove.size(); i++) {
        g_privateChats.erase(toRemove[i]);
    }
}

void DrawUI() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Appearing);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_MenuBar;

    if (!ImGui::Begin("MainChatRoom", nullptr, flags)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginMenuBar()) {
        ImGui::Text("JChat v1.0");
        ImGui::EndMenuBar();
    }

    ImGui::BeginChild("ConnectionPanel", ImVec2(0, 105), true, ImGuiWindowFlags_NoScrollbar);

    ImVec4 statusColor;
    if (g_network.connected) statusColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
    else if (g_network.status.find("Waiting") != std::string::npos || g_network.status.find("Running") != std::string::npos)
        statusColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
    else statusColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);

    ImGui::TextColored(statusColor, "%s", g_network.status.c_str());

    if (g_network.type == ClientType::SERVER && g_network.isRunning) {
        ImGui::SameLine();
        std::lock_guard<std::mutex> lock(g_network.clientsMutex);
        ImGui::Text("| Clients: %d/%d", (int)g_network.clients.size(), MAX_CLIENTS);
    }

    ImGui::Separator();

    if (g_network.connected || g_network.isRunning) {
        ImGui::Text("Mode: %s | Nickname: %s | ID: [%d]",
            g_network.type == ClientType::SERVER ? "Server" : "Client",
            g_nickname,g_network.myid);

        if (g_network.type == ClientType::CLIENT) {
            ImGui::SameLine();
            ImGui::Text("| Server: %s", g_network.serverIP.c_str());
        }

        if (ImGui::Button("Disconnect", ImVec2(80, 20))) {
            Disconnect();
        }
        ImGui::SameLine();
        ImGui::Checkbox("BGM", &hasBGM);
        ImGui::SameLine();
        ImGui::Checkbox("SoundEffect", &soundEffect);
    }
    else {
        ImGui::InputText("Nickname", g_nickname, sizeof(g_nickname));
        ImGui::InputText("Server IP", g_serverIP, sizeof(g_serverIP));
        if (ImGui::Button("Connect", ImVec2(80, 20))) {
            starting = true;
            StartClient(g_serverIP);
        }
        ImGui::SameLine();
        if (!starting) {
            if (ImGui::Button("Host Server", ImVec2(80, 20))) {
                StartServer();
            }
        }  
        ImGui::SameLine();
        ImGui::Checkbox("BGM", &hasBGM);
        ImGui::SameLine();
        ImGui::Checkbox("SoundEffect", &soundEffect);
    }
    ImGui::EndChild();

    float userListWidth = 150.0f;
    float spacing = 8.0f;

    DrawUserListPanel(userListWidth, -40);

    ImGui::SameLine();

    ImGui::BeginChild("ChatArea", ImVec2(0, -40), true);
    {
        ImGui::BeginChild("ChatHistory", ImVec2(0, 0), false);
        {
            std::lock_guard<std::mutex> lock(g_msgMutex);
            for (int i = 0; i < g_messages.size(); i++) {
                const ChatMessage& msg = g_messages[i];
                ImVec4 color;
                if (msg.isSystem) color = ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
                else if (msg.isSelf) color = ImVec4(0.3f, 0.7f, 1.0f, 1.0f);
                else if (msg.sender.find("[Server]") != std::string::npos) color = ImVec4(0.5f, 1.0f, 0.5f, 1.0f);
                else color = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);

                ImGui::TextColored(color, "[%s] %s: %s",
                    msg.timestamp.c_str(), msg.sender.c_str(), msg.content.c_str());
            }
        }

        if (g_scrollToBottom) {
            ImGui::SetScrollHereY(1.0f);
            g_scrollToBottom = false;
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 4));
    float inputWidth = ImGui::GetContentRegionAvail().x - 55;
    ImGui::SetNextItemWidth(inputWidth);

    bool enterPressed = ImGui::InputText("##input", g_inputBuffer, sizeof(g_inputBuffer),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();

    bool btnClicked = ImGui::Button("Send", ImVec2(50, 0));

    if ((enterPressed || btnClicked) && strlen(g_inputBuffer) > 0) {
        std::string msg(g_inputBuffer);

        if (msg.substr(0, 6) == "!name ") {
            std::string newName = msg.substr(6);
            if (!newName.empty()) {
                HandleNameChangeCommand(newName);
            }
        }
        else if (g_network.connected || g_network.isRunning) {
            if (SendNetworkMessage(msg)) {
                if (g_network.type == ClientType::CLIENT) {
                    std::string me = "[Me]" + std::string(g_nickname);
                    AddMessage(me, msg, true);
                }
            }
        }
        else {
            AddMessage("System", "Not connected", false, true);
        }
        g_inputBuffer[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::PopStyleVar();
    ImGui::TextDisabled("Commands: !name <Nickname> = Change name | !list = Check user list | !quit = Exit");

    ImGui::End();

    DrawPrivateChatWindows();
}

//---------------------------------------------------------------------------------------------
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

//----------------------------------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    std::thread(playBGM, "Sounds/bgm").detach();
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        MessageBoxA(nullptr, "WinSock initialization failed", "Error", MB_OK);
        return 1;
    }
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,  GetModuleHandle(nullptr),
        nullptr, nullptr, nullptr, nullptr, L"JChat", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"JChat Version 1.0", WS_OVERLAPPEDWINDOW, 100, 100,
        (int)(800 * main_scale), (int)(600 * main_scale), nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_TitleBg] = ImVec4(0.0f, 0.0f, 0.3f, 1.0f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.9f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.2f, 0.4f, 0.6f, 1.0f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 1.0f, 1.0f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    bool done = false;
    while (!done) {
        MSG msg;
        if (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
            continue;
        }
        if (done) {
            break;
        }
        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawUI();

        ImGui::Render();
        const float clear_color_with_alpha[4]
            = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    Disconnect();
    systemRunning = false;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    WSACleanup();

    return 0;
}