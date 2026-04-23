#include "Edriel.hpp"
#include <iostream>


int main()
{
    // Protobuf test
    std::cout << "Protobuf version: " << GOOGLE_PROTOBUF_VERSION << std::endl;
    // gRPC test
    std::cout << "gRPC version: " << grpc::Version() << std::endl;
    // Asio test
    asio::io_context io;
    std::cout << "Asio io_context created." << std::endl;
    edriel::Edriel edriel(io);
    edriel.startAutoDiscovery();
	io.run();
    std::cout << "io_context run() has exited." << std::endl;

	return 0;
}
