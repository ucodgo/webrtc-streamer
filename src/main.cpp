/* ---------------------------------------------------------------------------
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** main.cpp
**
** -------------------------------------------------------------------------*/

#include <signal.h>

#include <iostream>
#include <fstream>

#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#include "p2p/base/stun_server.h"
#include "p2p/base/basic_packet_socket_factory.h"
#include "p2p/base/turn_server.h"

#include "system_wrappers/include/field_trial.h"

#include "PeerConnectionManager.h"
#include "HttpServerRequestHandler.h"

#include "cxxopts.hpp"

PeerConnectionManager* webRtcServer = NULL;

void sighandler(int n)
{
	printf("SIGINT\n");
	// delete need thread still running
	delete webRtcServer;
	webRtcServer = NULL;
	rtc::Thread::Current()->Quit(); 
}

/* ---------------------------------------------------------------------------
**  main
** -------------------------------------------------------------------------*/
int main(int argc, char* argv[])
{
	std::string stunurl       = "stun.l.google.com:19302";
	const char* defaultlocalstunurl  = "0.0.0.0:3478";
	std::string localstunurl;
	std::string turnurl;
	const char* defaultlocalturnurl  = "turn:turn@0.0.0.0:3478";
	std::string localturnurl;
	std::string webrtcUdpPortRange = "0:65535";
	int logLevel              = rtc::LERROR;
	std::string webroot       = "./html";
	std::string sslCertificate;
	int audioLayer = webrtc::AudioDeviceModule::kPlatformDefaultAudio;
	std::string streamName;
	std::string nbthreads;
	std::string passwdFile;
	std::string authDomain = "mydomain.com";
	std::string publishFilter(".*");
	Json::Value config;  
	bool        useNullCodec = false;

	std::string httpAddress("0.0.0.0:");
	std::string httpPort = "8000";
	const char * port = getenv("PORT");
	if (port)
	{
		httpPort = port;
	}
	httpAddress.append(httpPort);


    cxxopts::Options options(argv[0], "WebRTC Streamer");

    options.add_options()
		("h,help", "Print usage")
		("V,version", "print version")
		("v,verbose", "set verbosity level", cxxopts::value<int>(logLevel)->implicit_value(std::to_string(rtc::WARNING)))

		("C,config", "load urls from JSON config file", cxxopts::value<std::string>())
		("o,nullcodec", "use null codec (keep frame encoded)", cxxopts::value<bool>(useNullCodec))
		("a,audiolayer", "spefify audio capture layer to use", cxxopts::value<int>(audioLayer)->implicit_value(std::to_string(webrtc::AudioDeviceModule::kDummyAudio)))
		("q,publishfilter", "spefify publish filter", cxxopts::value<std::string>()->default_value(publishFilter))

		("R,udprange", "Set the webrtc udp port range", cxxopts::value<std::string>()->default_value(webrtcUdpPortRange))
		("S,localstun", "start embeded STUN server bind to address", cxxopts::value<std::string>()->implicit_value(defaultlocalstunurl))
		("s,stun", "use a STUN server (- means no STUN)", cxxopts::value<std::string>()->default_value(stunurl))
		("T,localturn", "start embeded TURN server bind to address (default:disabled)", cxxopts::value<std::string>()->implicit_value(defaultlocalturnurl))
		("t,turn", "use a TURN server (default:disabled)", cxxopts::value<std::string>())

        ("H,httpaddress", "HTTP server binding", cxxopts::value<std::string>()->default_value(httpAddress))
        ("c,cert", "path to private key and certificate for HTTPS", cxxopts::value<std::string>())
		("w,webroot", "path to get static files", cxxopts::value<std::string>()->default_value(webroot))
		("N,nbthreads", "number of threads for HTTP server", cxxopts::value<std::string>())
		("A,passwd", "password file for HTTP server access", cxxopts::value<std::string>())
		("D,domain", "authentication domain for HTTP server access", cxxopts::value<std::string>()->default_value(authDomain))
    ;

    cxxopts::ParseResult result = options.parse(argc, argv);
    if (result.count("help")) {
      std::cout << options.help() << std::endl;
      exit(0);
    }
    if (result.count("version")) {
      std::cout << VERSION << std::endl;
      exit(0);
    }
    if (result.count("verbose")) {
		logLevel=result["verbose"].as<int>(); 
    }	
    if (result.count("config")) {
		std::ifstream stream(result["config"].as<std::string>());
		stream >> config;
    }	
	httpAddress = result["httpaddress"].as<std::string>();
	if (result.count("cert")) {
		sslCertificate = result["cert"].as<std::string>();
	}
	webroot = result["webroot"].as<std::string>();
	if (result.count("nbthreads")) {	
		nbthreads = result["nbthreads"].as<std::string>();
	}
	authDomain = result["domain"].as<std::string>();

	publishFilter = result["publishfilter"].as<std::string>();
	if (result.count("nullcodec")) {
		useNullCodec = result["nullcodec"].as<bool>();
	}
	if (result.count("audio")) {
		audioLayer = result["audio"].as<int>();
	}		
	if (result.count("udprange")) {
		webrtcUdpPortRange = result["udprange"].as<std::string>();
	}	
	if (result.count("localstun")) {
		localstunurl = result["localstun"].as<std::string>();
		stunurl = localstunurl;
	}	
	if (result.count("stun")) {
		stunurl = result["stun"].as<std::string>();
	}	
	if (result.count("localturn")) {
		localturnurl = result["localturn"].as<std::string>();
		turnurl = localturnurl;
	}			
	if (result.count("turn")) {
		turnurl = result["turn"].as<std::string>();
	}	

	auto arguments = result.arguments();
    std::cout << "Saw " << arguments.size() << " arguments" << std::endl;

	std::cout  << "Version:" << VERSION << std::endl;

	std::cout  << config;

	rtc::LogMessage::LogToDebug((rtc::LoggingSeverity)logLevel);
	rtc::LogMessage::LogTimestamps();
	rtc::LogMessage::LogThreads();
	std::cout << "Logger level:" <<  rtc::LogMessage::GetLogToDebug() << std::endl;

	rtc::Thread* thread = rtc::Thread::Current();
	rtc::InitializeSSL();

	// webrtc server
	std::list<std::string> iceServerList;
	if ((stunurl.size() != 0) && (stunurl != "-")) {
		iceServerList.push_back(std::string("stun:")+stunurl);
	}
	if (turnurl.size() != 0) {
		iceServerList.push_back(std::string("turn:")+turnurl);
	}

	// init trials fields
	webrtc::field_trial::InitFieldTrialsFromString("WebRTC-FrameDropper/Disabled/");

	webRtcServer = new PeerConnectionManager(iceServerList, config["urls"], (webrtc::AudioDeviceModule::AudioLayer)audioLayer, publishFilter, webrtcUdpPortRange, useNullCodec);
	if (!webRtcServer->InitializePeerConnection())
	{
		std::cout << "Cannot Initialize WebRTC server" << std::endl;
	}
	else
	{
		// http server
		std::vector<std::string> options;
		options.push_back("document_root");
		options.push_back(webroot);
		options.push_back("enable_directory_listing");
		options.push_back("no");
		options.push_back("additional_header");
		options.push_back("X-Frame-Options: SAMEORIGIN");
		options.push_back("access_control_allow_origin");
		options.push_back("*");
		options.push_back("listening_ports");
		options.push_back(httpAddress);
		options.push_back("enable_keep_alive");
		options.push_back("yes");
		options.push_back("keep_alive_timeout_ms");
		options.push_back("1000");
		options.push_back("decode_url");
		options.push_back("no");
		if (!sslCertificate.empty()) {
			options.push_back("ssl_certificate");
			options.push_back(sslCertificate);
		}
		if (!nbthreads.empty()) {
			options.push_back("num_threads");
			options.push_back(nbthreads);
		}
		if (!passwdFile.empty()) {
			options.push_back("global_auth_file");
			options.push_back(passwdFile);
			options.push_back("authentication_domain");
			options.push_back(authDomain);
		}
		
		try {
			std::map<std::string,HttpServerRequestHandler::httpFunction> func = webRtcServer->getHttpApi();
			std::cout << "HTTP Listen at " << httpAddress << std::endl;
			HttpServerRequestHandler httpServer(func, options);

			// start STUN server if needed
			std::unique_ptr<cricket::StunServer> stunserver;
			if (!localstunurl.empty())
			{
				rtc::SocketAddress server_addr;
				server_addr.FromString(localstunurl);
				rtc::AsyncUDPSocket* server_socket = rtc::AsyncUDPSocket::Create(thread->socketserver(), server_addr);
				if (server_socket)
				{
					stunserver.reset(new cricket::StunServer(server_socket));
					std::cout << "STUN Listening at " << server_addr.ToString() << std::endl;
				}
			}

			// start TRUN server if needed
			std::unique_ptr<cricket::TurnServer> turnserver;
			if (!localturnurl.empty())
			{
				std::istringstream is(localturnurl);
				std::string addr;
				std::getline(is, addr, '@');
				std::getline(is, addr, '@');
				rtc::SocketAddress server_addr;
				server_addr.FromString(addr);
				turnserver.reset(new cricket::TurnServer(rtc::Thread::Current()));

				rtc::AsyncUDPSocket* server_socket = rtc::AsyncUDPSocket::Create(thread->socketserver(), server_addr);
				if (server_socket)
				{
					std::cout << "TURN Listening UDP at " << server_addr.ToString() << std::endl;
					turnserver->AddInternalSocket(server_socket, cricket::PROTO_UDP);
				}
				rtc::AsyncSocket* tcp_server_socket = thread->socketserver()->CreateAsyncSocket(AF_INET, SOCK_STREAM);
				if (tcp_server_socket) {
					std::cout << "TURN Listening TCP at " << server_addr.ToString() << std::endl;
					tcp_server_socket->Bind(server_addr);
					tcp_server_socket->Listen(5);
					turnserver->AddInternalServerSocket(tcp_server_socket, cricket::PROTO_TCP);
				}

				is.str(turnurl);
				is.clear();
				std::getline(is, addr, '@');
				std::getline(is, addr, '@');
				rtc::SocketAddress external_server_addr;
				external_server_addr.FromString(addr);		
				std::cout << "TURN external addr:" << external_server_addr.ToString() << std::endl;			
				turnserver->SetExternalSocketFactory(new rtc::BasicPacketSocketFactory(), rtc::SocketAddress(external_server_addr.ipaddr(), 0));
			}
			
			// mainloop
			signal(SIGINT,sighandler);
			thread->Run();

		} catch (const CivetException & ex) {
			std::cout << "Cannot Initialize start HTTP server exception:" << ex.what() << std::endl;
		}
	}

	rtc::CleanupSSL();
	std::cout << "Exit" << std::endl;
	return 0;
}

