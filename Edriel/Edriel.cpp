// Edriel.cpp : 애플리케이션의 진입점을 정의합니다.
//

#include "Edriel.hpp"


int main()
{
    // Protobuf test
    std::cout << "Protobuf version: " << GOOGLE_PROTOBUF_VERSION << std::endl;
    // gRPC test
    std::cout << "gRPC version: " << grpc::Version() << std::endl;
    // Asio test
    asio::io_context io;
    std::cout << "Asio io_context created." << std::endl;
    Edriel edriel(io);
	io.run();
    std::cout << "io_context run() has exited." << std::endl;

	return 0;
}
