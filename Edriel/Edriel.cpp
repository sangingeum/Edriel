// Edriel.cpp : 애플리케이션의 진입점을 정의합니다.
//

#include "Edriel.hpp"
#include <asio.hpp>
#include <google/protobuf/stubs/common.h>
#include <grpcpp/grpcpp.h>
#include <iostream>

using namespace std;

int main()
{
    // Asio test
    asio::io_context io;
    cout << "Asio io_context created." << endl;

    // Protobuf test
    cout << "Protobuf version: " << GOOGLE_PROTOBUF_VERSION << endl;

    // gRPC test
    cout << "gRPC version: " << grpc::Version() << endl;

	return 0;
}
