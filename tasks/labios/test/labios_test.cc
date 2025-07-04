#include "labios/labios_client.h"
#include <random>
#include <cstring>
#include <mpi.h>
#include <iostream>

std::string generate_random_data(size_t size_bytes) {
    // Create a random engine with a random seed
    std::random_device rd;
    std::mt19937 gen(rd());
    
    // Define the range of printable ASCII characters (33-126)
    std::uniform_int_distribution<> dist(33, 126);
    
    // Generate random data
    std::stringstream buffer;
    for (size_t i = 0; i < size_bytes; ++i) {
        buffer << static_cast<char>(dist(gen));
    }
    
    return buffer.str();
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    std::cout << "Process " << rank << " of " << size << std::endl;

    std::cout << "LABIOS started by Rajni" << std::endl;
    CHIMAERA_CLIENT_INIT();
    chi::labios::Client client;
    client.Create(
        HSHM_MCTX,
        chi::DomainQuery::GetDirectHash(chi::SubDomainId::kGlobalContainers, 0),
        chi::DomainQuery::GetGlobalBcast(), "ipc_test");
        
    std::cout << "LABIOS container created successfully" << std::endl;

    size_t data_size = 1024;  // 1MB
    std::string test_data = generate_random_data(data_size);

    hipc::FullPtr<char> orig_data = CHI_CLIENT->AllocateBuffer(HSHM_MCTX, test_data.size() + 1);
    std::memcpy(orig_data.ptr_, test_data.c_str(), test_data.size() + 1);

    hipc::FullPtr<char> new_data = CHI_CLIENT->AllocateBuffer(HSHM_MCTX, test_data.size() + 1);

    std::string key = "first_key";
    auto query = chi::DomainQuery::GetDynamic();
    auto loc = chi::DomainQuery::GetDirectId(chi::SubDomain::kGlobalContainers,0,0);

    client.MdGetOrCreate(HSHM_MCTX, query, key, 0, data_size + 1, loc);

    client.Write(HSHM_MCTX, loc, key, 
                orig_data.shm_, 
                test_data.size() + 1);
    std::cout << "Write operation attempted" << std::endl;

    client.Read(HSHM_MCTX, 
                loc, key, new_data.shm_, test_data.size() + 1);
    std::cout << "Input Data: " << test_data << std::endl;
    std::cout << "Read back: " << new_data.ptr_ << std::endl;

    MPI_Finalize();
    return 0;
}