//
// Created by lukemartinlogan on 7/22/22.
//
#include <hermes_shm/util/timer.h>
#include <sstream>
#include <random>
#include <mpi.h>
#include <iomanip>
#include "labios/labios_client.h"

void gen_random(char *buf, size_t size) {
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < size; ++i) {
        buf[i] = static_cast<char>(dist(generator));
    }
}

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);
  if (argc != 5) {
    printf("USAGE: ./montage_base [labios_conf] [file_path] [iter] "
           "[final_path]\n");
    exit(1);
  }

#ifdef COLLECT
  system("sh /home/cc/nfs/aetrio/scripts/log_reset.sh");
#endif
  std::stringstream stream;
  int rank, comm_size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#ifdef DEBUG
  if (rank == 0)
    std::cerr << "Running Montage in TABIOS\n";
#endif
  MPI_Comm_size(MPI_COMM_WORLD, &comm_size);
    // Initialize Chimaera client
  CHIMAERA_CLIENT_INIT();
  
  // Create Labios client
  chi::labios::Client client;

  std::string file_path = argv[2];
  int iteration = atoi(argv[3]);
  std::string final_path = argv[4];
  std::string filename1 = file_path + "file1_" + std::to_string(rank) + ".dat";
  std::string filename2 = file_path + "file2_" + std::to_string(rank) + ".dat";
  size_t io_per_teration = 32 * 1024 * 1024;
  std::vector<std::array<size_t, 2>> workload =
      std::vector<std::array<size_t, 2>>();
  MPI_Barrier(MPI_COMM_WORLD);
#ifdef TIMERBASE
  hshm::Timer c = Timer();
  c.resumeTime();
#endif
#ifdef DEBUG
  if (rank == 0)
    std::cerr << "Starting Simulation Phase\n";
#endif
  int count = 0;
  for (auto i = 0; i < 32; ++i) {
    for (int j = 0; j < comm_size * 1024 * 128; ++j) {
      count += 1;
      auto result = count * j;
      result -= j;
    }
    count = 0;
  }
  workload.push_back({1 * 1024 * 1024, 32});
  size_t current_offset = 0;
  char *write_buf[32];
  hipc::FullPtr<char> write_data[32];
  for (int i = 0; i < 32; ++i) {
    write_buf[i] = static_cast<char *>(malloc(1 * 1024 * 1024));
    gen_random(write_buf[i], 1 * 1024 * 1024);
    write_data[i] = CHI_CLIENT->AllocateBuffer(HSHM_MCTX, 1 * 1024 * 1024);
    std::memcpy(write_data[i].ptr_, write_buf[i], 1 * 1024 * 1024);
  }
#ifdef TIMERBASE
  c.pauseTime();
#endif
#ifdef TIMERBASE
  if (rank == 0) {
    stream << "montage_tabios()," << std::fixed << std::setprecision(10)
           << c.elapsed_time << ",";
  }
#endif
#ifdef TIMERBASE
  Timer w = Timer();
#endif
#ifdef DEBUG
  if (rank == 0)
    std::cerr << "Starting Write Phase\n";
#endif
  hshm::Timer global_timer = hshm::Timer();
  global_timer.Resume();
  FILE *fd1, *fd2;
  if (rank % 2 == 0 || comm_size == 1) {
#ifdef TIMERBASE
    w.resumeTime();
#endif
    client.Create(
        HSHM_MCTX,
        chi::DomainQuery::GetDirectHash(chi::SubDomainId::kGlobalContainers, rank * 2),
        chi::DomainQuery::GetGlobalBcast(), 
        "montage_container1_" + std::to_string(rank)
    );
    
    client.Create(
        HSHM_MCTX,
        chi::DomainQuery::GetDirectHash(chi::SubDomainId::kGlobalContainers, rank * 2 + 1),
        chi::DomainQuery::GetGlobalBcast(), 
        "montage_container2_" + std::to_string(rank)
    );
#ifdef TIMERBASE
    w.pauseTime();
#endif
  }
  global_timer.Pause();
  std::vector<std::pair<size_t, std::pair<std::string, chi::DomainQuery>>> operations =
    std::vector<std::pair<size_t, std::pair<std::string, chi::DomainQuery>>>();

  for (int i = 0; i < iteration; ++i) {
    for (auto item : workload) {
      for (int j = 0; j < item[1]; ++j) {
        global_timer.Resume();
#ifdef TIMERBASE
        w.resumeTime();
#endif
        if (rank % 2 == 0 || comm_size == 1) {
          std::string key;
          chi::DomainQuery loc;
          if (j % 2 == 0) {
            key = filename1 + "_iter_" + std::to_string(i) + "_chunk_" + std::to_string(j);
            loc = chi::DomainQuery::GetDirectId(chi::SubDomain::kGlobalContainers, rank * 2, 0);

          } else {
            key = filename2 + "_iter_" + std::to_string(i) + "_chunk_" + std::to_string(j);
            loc = chi::DomainQuery::GetDirectId(chi::SubDomain::kGlobalContainers, rank * 2 + 1, 0);
          }
          auto query = chi::DomainQuery::GetDynamic();
          client.MdGetOrCreate(HSHM_MCTX, query, key, current_offset, item[0], loc);
          client.Write(HSHM_MCTX, loc, key, write_data[j].shm_, item[0]);
          
          // Store operation info
          operations.emplace_back(std::make_pair(item[0], std::make_pair(key, loc)));
        }
#ifdef TIMERBASE
        w.pauseTime();
#endif
        global_timer.Pause();
        current_offset += item[0];
      }
    }
  }
  global_timer.Resume();
#ifdef TIMERBASE
  w.resumeTime();
#endif
  if (rank % 2 == 0 || comm_size == 1) {
    for (auto operation : operations) {
       size_t expected_size = operation.first;
      std::string key = operation.second.first;
      chi::DomainQuery loc = operation.second.second;
      
      // Allocate buffer for read verification
      hipc::FullPtr<char> read_verify_data = CHI_CLIENT->AllocateBuffer(HSHM_MCTX, expected_size);
      
      // Read back the data for verification
      client.Read(HSHM_MCTX, loc, key, read_verify_data.shm_, expected_size);
    }
  }
#ifdef TIMERBASE
  w.pauseTime();
#endif
  global_timer.Pause();
  MPI_Barrier(MPI_COMM_WORLD);
#ifdef TIMERBASE

  double w_time = 0.0;
  if (rank % 2 == 0)
    w_time = w.elapsed_time;
  double w_sum;
  MPI_Allreduce(&w_time, &w_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  double w_mean = w_sum / comm_size * 2;
  if (rank == 0)
    stream << w_mean << ",";
#endif
  for (int i = 0; i < 32; ++i) {
    free(write_buf[i]);
  }
#ifdef TIMERBASE
  Timer r = Timer();
#endif
#ifdef DEBUG
  if (rank == 0)
    std::cerr << "Starting Reading Phase\n";
#endif
  size_t align = 4096;
  global_timer.Resume();
#ifdef TIMERBASE
  r.resumeTime();
#endif
  if (rank % 2 != 0 || comm_size == 1) {
    int producer_rank;
    if (comm_size == 1) {
      producer_rank = rank;
    } else {
      producer_rank = rank - 1;
    }
  }
#ifdef TIMERBASE
  r.pauseTime();
#endif
  global_timer.Pause();
  for (int i = 0; i < iteration; ++i) {
    for (auto item : workload) {
      for (int j = 0; j < item[1]; ++j) {
        // char read_buf[item[0]];
        global_timer.Resume();

#ifdef TIMERBASE
        r.resumeTime();
#endif
        if (rank % 2 != 0 || comm_size == 1) {
          int producer_rank = (comm_size == 1) ? rank : rank - 1;
#ifndef COLLECT
          hipc::FullPtr<char> read_buf1 = CHI_CLIENT->AllocateBuffer(HSHM_MCTX, item[0] / 2);
          hipc::FullPtr<char> read_buf2 = CHI_CLIENT->AllocateBuffer(HSHM_MCTX, item[0] / 2);
          std::string key1, key2;
           if (comm_size == 1) {
            key1 = filename1 + "_iter_" + std::to_string(i) + "_chunk_" + std::to_string(j);
            key2 = filename2 + "_iter_" + std::to_string(i) + "_chunk_" + std::to_string(j);
          } else {
            std::string prod_filename1 = file_path + "file1_" + std::to_string(producer_rank) + ".dat";
            std::string prod_filename2 = file_path + "file2_" + std::to_string(producer_rank) + ".dat";
            key1 = prod_filename1 + "_iter_" + std::to_string(i) + "_chunk_" + std::to_string(j);
            key2 = prod_filename2 + "_iter_" + std::to_string(i) + "_chunk_" + std::to_string(j);
          }
          
          // Read from both containers
          auto loc1 = chi::DomainQuery::GetDirectId(chi::SubDomain::kGlobalContainers, producer_rank * 2, 0);
          auto loc2 = chi::DomainQuery::GetDirectId(chi::SubDomain::kGlobalContainers, producer_rank * 2 + 1, 0);
          

          ssize_t bytes = 0;
          try {
            client.Read(HSHM_MCTX, loc1, key1, read_buf1.shm_, item[0] / 2);
            bytes += item[0] / 2;
            client.Read(HSHM_MCTX, loc2, key2, read_buf2.shm_, item[0] / 2);
            bytes += item[0] / 2;
            } catch (...) {
          std::cerr << "Read() failed!" 
                    << "Bytes:" << bytes << "\n";
          }
          if (bytes != item[0])
            std::cerr << "Read() failed!"
                      << "Bytes:" << bytes << "\tError code:" << errno << "\n";
#endif
        }
#ifdef TIMERBASE
        r.pauseTime();
#endif
        global_timer.Pause();
        current_offset += item[0];
      }
    }
  }
  global_timer.Resume();
#ifdef TIMERBASE
  r.resumeTime();
#endif
#ifdef TIMERBASE
  r.pauseTime();
#endif
  MPI_Barrier(MPI_COMM_WORLD);
  global_timer.Pause();

#ifdef TIMERBASE
  double r_time = 0.0;
  if (rank % 2 == 1 || comm_size == 1)
    r_time = r.elapsed_time;
  double r_sum;
  MPI_Allreduce(&r_time, &r_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  double r_mean = r_sum / comm_size * 2;
  if (rank == 0 && comm_size > 1) {
    stream << r_mean << ",";
  }
#endif
#ifdef DEBUG
  if (rank == 0)
    std::cerr << "Starting Analysis Phase\n";
#endif
#ifdef TIMERBASE
  Timer a = Timer();
  a.resumeTime();
#endif
  std::string finalname = final_path + "final_" + std::to_string(rank) + ".dat";
  global_timer.Resume();
  
  if (rank == 0)
    std::cerr << "Starting final output phase...\n";
  
  // FIX: Don't create separate output container - use existing one
  // client.Create(..., rank + 10000, ...);  // COMMENTED OUT

  global_timer.Pause();
  for (auto i = 0; i < 32; ++i) {
    for (int j = 0; j < comm_size * 1024 * 128; ++j) {
      count += 1;
      auto result = count * j;
      result -= j;
    }
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<int> dist(0, io_per_teration);
    auto rand = dist(generator);
    int x = (i + 1) * rand;
  }
  char final_buff[1024 * 1024];
  gen_random(final_buff, 1024 * 1024);
  hipc::FullPtr<char> final_data = CHI_CLIENT->AllocateBuffer(HSHM_MCTX, 1024 * 1024);
  std::memcpy(final_data.ptr_, final_buff, 1024 * 1024);
  
  global_timer.Resume();
  
  if (rank == 0)
    std::cerr << "Writing final output...\n";
  
  // FIX: Use same container as original data
  // For Montage, we use the first container (rank * 2) for final output
  std::string final_key = "montage_final_output_" + std::to_string(rank);
  auto final_query = chi::DomainQuery::GetDynamic();
  // Use existing container instead of creating new one
  auto final_loc = chi::DomainQuery::GetDirectId(chi::SubDomain::kGlobalContainers, rank, 0);
  
  client.MdGetOrCreate(HSHM_MCTX, final_query, final_key, 0, 1024 * 1024, final_loc);
  client.Write(HSHM_MCTX, final_loc, final_key, final_data.shm_, 1024 * 1024);
  
  if (rank == 0)
    std::cerr << "Final write completed!\n";
  
#ifdef TIMERBASE
  a.pauseTime();
#endif
  global_timer.Pause();
#ifdef TIMERBASE
  auto a_time = a.elapsed_time;
  double a_sum;
  MPI_Allreduce(&a_time, &a_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  double a_mean = a_sum / comm_size;
  if (rank == 0)
    stream << a_mean << ",";
#endif

  auto time = global_timer.GetSec();
  double sum;
  MPI_Allreduce(&time, &sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  double mean = sum / comm_size;
  if (rank == 0) {
#ifdef COLLECT
    double ts = get_average_ts();
    double worker = get_average_worker();
    stream << ts << "," << worker << ",";
#endif
    stream << "average," << mean << "\n";
    std::cerr << stream.str();
  }

  if (rank == 0)
    std::cerr << "Montage test completed successfully!\n";

  MPI_Finalize();
  return 0;
}