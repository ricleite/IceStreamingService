#include <sstream>
#include <csignal>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netdb.h>

#include "Streamer.h"
#include "Util.h"

#define LISTEN_BACKLOG 10
#define BUFFER_SIZE 256

using namespace StreamingService;

void exitHandler(int signal);

// need a global to handle Ctrl-C interrupts
bool early_exit = false;

int main(int argc, char** argv)
{
    // catch Ctrl-C, need to remove stream from portal
    signal(SIGINT, exitHandler);

    Streamer app;
    return app.main(argc, argv, "config.streamer");
}

void exitHandler(int /*signal*/)
{
    LOG_INFO("Exiting...");
    early_exit = true;
}

Streamer::Streamer() : Ice::Application(Ice::NoSignalHandling) { }

int Streamer::run(int argc, char** argv)
{
    if (argc < 3)
    {
        PrintUsage();
        return 1;
    }

    IceInternal::Application::_signalPolicy = Ice::NoSignalHandling;

    _videoFilePath = argv[1];
    std::string streamName = argv[2];
    _transport = "tcp";
    _host = "localhost";
    _listenPort = 9600;
    _ffmpegPort = 9601;
    std::string videoSize = "480x270";
    std::string bitRate = "400k";
    std::string keywords; // actually a list with csv values

    // parse command line options
    for (int i = 3; i < argc; ++i)
    {
        std::string option = argv[i];

        // all options have a following arg
        if (i + 1 >= argc)
        {
            LOG_INFO("Missing argument after option %s", option.c_str());
            return 1;
        }

        std::string arg = argv[i + 1];

        if (option == "--transport")
            _transport = arg;
        else if (option == "--host")
            _host = arg;
        else if (option == "--port")
            _listenPort = atoi(arg.c_str());
        else if (option == "--ffmpeg_port")
            _ffmpegPort = atoi(arg.c_str());
        else if (option == "--video_size")
            videoSize = arg;
        else if (option == "--bit_rate")
            bitRate = arg;
        else if (option == "--keywords")
            keywords = arg;
        else
            LOG_INFO("Unrecognized option '%s', skipping", option.c_str());
    }

    // setup stream entry
    // endpoint format: transport://host:port
    std::string endpoint = _transport +
        "://" + _host +
        ":" + std::to_string(_listenPort);

    _streamEntry.streamName = streamName;
    _streamEntry.endpoint = endpoint;
    _streamEntry.videoSize = videoSize;
    _streamEntry.bitRate = bitRate;
    // fill stream keywords
    {
        std::string t;
        std::stringstream ss(keywords);
        while (std::getline(ss, t, ','))
            _streamEntry.keyword.push_back(t);
    }

    int exitCode = 0;

    // actual stream logic
    {
        // open listen port, start ffmpeg
        if (Initialize())
        {
            // start stream
            Run();
        }
        else
        {
            LOG_ERROR("Streamer initialization failed");
            exitCode = 1;
        }
    }

    // close and cleanup
    Close();
    return exitCode;
}

bool Streamer::Initialize()
{
    Ice::ObjectPrx base = communicator()->propertyToProxy("Portal.Proxy");
    _portal = PortalInterfacePrx::checkedCast(base);

    if (!_portal)
    {
        LOG_ERROR("failed to find portal");
        return false;
    }

    // open listen port
    {
        LOG_INFO("Setting up listen socket...");

        _listenSocketFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (_listenSocketFd < 0)
        {
            LOG_ERROR("Failed to initialize listen socket");
            return false;
        }

        sockaddr_in addr;
        bzero((char*)&addr, sizeof(addr));

        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(_listenPort);

        if (bind(_listenSocketFd, (sockaddr*)&addr, sizeof(addr)) < 0)
        {
            LOG_ERROR("Failed to bind listen socket");
            return false;
        }

        if (listen(_listenSocketFd, LISTEN_BACKLOG) < 0)
        {
            LOG_ERROR("Failed to open listen socket");
            return false;
        }

        int setVal = 1;
        setsockopt(_listenSocketFd, SOL_SOCKET, SO_REUSEADDR, &setVal, sizeof(int));
    }

    // start ffmpeg, wait for open port
    {
        // ffmpeg necessarily starts on localhost, only port can change
        std::string ffmpegHost = "127.0.0.1";

        // need to setup a seperate endpoint for ffmpeg, since the port will differ
        std::string endpoint = _transport +
            "://" + ffmpegHost +
            ":" + std::to_string(_ffmpegPort);

        LOG_INFO("Starting and connecting to ffmpeg...");

        _ffmpegPid = fork();
        if (_ffmpegPid == 0)
        {
            // for the sake of flexibility, a shell script is used
            // it's better than coding all ffmpeg arguments
            // arguments used:
            // $1 = video file path
            // $2 = end point info in "transport://ip:port" format (e.g tcp://127.0.0.1:999$
            // $3 = video size (e.g 420x320)
            // $4 = video bitrate (e.g 400k or 400000)
            execlp("./streamer_ffmpeg.sh", "streamer_ffmpeg.sh",
                _videoFilePath.c_str(),             // $1
                endpoint.c_str(),                   // $2
                _streamEntry.videoSize.c_str(),     // $3
                _streamEntry.bitRate.c_str(),       // $4
                nullptr);
        }

        _ffmpegSocketFd = socket(AF_INET, SOCK_STREAM, 0);
        hostent* server = gethostbyname(ffmpegHost.c_str());

        sockaddr_in addr;
        bzero((char*)&addr, sizeof(addr));
        addr.sin_family = AF_INET;
        bcopy((char*)server->h_addr, (char*)&addr.sin_addr.s_addr, server->h_length);
        addr.sin_port = htons(_ffmpegPort);

        while (true)
        {
            if (early_exit)
            {
                LOG_INFO("Exiting early...");
                return false;
            }

            // @todo: socket won't connect if ffmpeg had an early exit, handle that
            int error = connect(_ffmpegSocketFd, (sockaddr*)&addr, sizeof(addr));
            if (error >= 0)
                break; // no error, finally have a valid socket

            usleep(500 * 1e3); // 500ms sleep
        }
    }

    _portal->NewStream(_streamEntry);
    return true;
}

void Streamer::Close()
{
    while (!_clientList.empty())
    {
        int clientSocket = _clientList.front();
        _clientList.pop_front();
        close(clientSocket);
    }

    if (_listenSocketFd > 0)
    {
        shutdown(_listenSocketFd, SHUT_RDWR);
        close(_listenSocketFd);
    }

    if (_ffmpegSocketFd > 0)
    {
        shutdown(_ffmpegSocketFd, SHUT_RDWR);
        close(_ffmpegSocketFd);
    }

    if (_portal)
        _portal->CloseStream(_streamEntry);

    if (_ffmpegPid > 0)
    {
        LOG_INFO("Sending SIGTERM to ffmpeg...");
        kill(_ffmpegPid, SIGTERM);

        LOG_INFO("Waiting on ffmpeg to exit...");
        wait(NULL);
    }
}

void Streamer::Run()
{
    LOG_INFO("Streamer ready");

    long const sleepTime = 20; // 20ms sleep time per cycle
    long const tickTimer = 30; // 30ms for sending data per cycle

    while (true)
    {
        // periodically accept new clients
        int clientSocket = accept4(_listenSocketFd, NULL, NULL, SOCK_NONBLOCK);
        if (clientSocket > 0)
        {
            _clientList.push_back(clientSocket);
            LOG_INFO("Accepted new client, fd %d", clientSocket);
        }

        usleep(sleepTime * 1e3); // wait a bit so there's some data to send

        long timeBeforeTick = getMSTime();

        // read from ffmpeg and send data
        // ffmpeg will produce data to be read at the right video play speed
        while (true)
        {
            char buffer[BUFFER_SIZE];
            ssize_t remaining = BUFFER_SIZE;
            while (remaining > 0)
            {
                if (early_exit)
                    return;

                size_t offset = BUFFER_SIZE - remaining;
                ssize_t n = read(_ffmpegSocketFd, buffer + offset, remaining);
                if (n < 0)
                {
                    LOG_ERROR("ffmpeg socket read failed");
                    return;
                }

                remaining -= n;
            }

            // send data to all clients, remove clients with invalid/closed sockets
            _clientList.remove_if([buffer](int clientSocket)
            {
                if (write(clientSocket, buffer, BUFFER_SIZE) < 0)
                {
                    LOG_INFO("Removing client fd %d from client list", clientSocket);
                    return true;
                }

                return false;
            });

            // break out of send cycle and accept new clients if a tick has passed
            long now = getMSTime();
            if (now - timeBeforeTick > tickTimer)
                break;
        }
    }
}

void Streamer::PrintUsage()
{
    LOG_INFO("Usage: ./streamer $video_file $stream_name [options]");
    LOG_INFO("Options:");
    LOG_INFO("'--transport $trans' sets endpoint transport protocol, tcp by default");
    LOG_INFO("'--host $host' sets endpoint host, localhost by default");
    LOG_INFO("'--port $port' specifies listen port, 9600 by default");
    LOG_INFO("'--ffmpeg_port $port' sets port for ffmpeg instance, 9601 by default");
    LOG_INFO("'--video_size $size' specifies video size, 480x270 by default");
    LOG_INFO("'--bit_rate $rate' sets video bit rate, 400k by default");
    LOG_INFO("'--keywords $key1,$key2...,$keyn' adds search keywords to stream");
}