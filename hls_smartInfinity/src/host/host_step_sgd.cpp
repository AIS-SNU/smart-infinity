/*
# Copyright (C) 2023, Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: X11
*/

#define CL_HPP_CL_1_2_DEFAULT_BUILD
#define CL_HPP_TARGET_OPENCL_VERSION 120
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#define CL_HPP_ENABLE_PROGRAM_CONSTRUCTION_FROM_ARRAY_COMPATIBILITY 1
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS

#include <vector>
#include <CL/cl2.hpp>
#include <iostream>
#include <fstream>
#include <CL/cl_ext_xilinx.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <cmath>
#include <iomanip>
#include <unistd.h>
#include <fcntl.h>
#include <cassert>
#include <CL/cl_ext.h>


static const float scale = 0.03;
static const int DATA_SIZE = 4096* 4096*2;

float _alpha = 1e-3;
float _betta = 0.9;
float _betta_minus1 = 1 - _betta;
float _weight_decay = 0.001;
float step_size = -1 * _alpha;
float w_decay = _weight_decay;

std::string param_name = "/mnt/smartssd1/param.tensor.swp";
std::string grad_name = "/mnt/smartssd1/grad.tensor.swp";
std::string exp_avg_sq_name = "/mnt/smartssd1/exp_avg_sq.tensor.swp";
std::string exp_avg_name = "/mnt/smartssd1/exp_avg.tensor.swp";

#include <x86intrin.h>
typedef ushort  half;
	
static const std::string error_message =
    "Error: Result mismatch:\n"
    "i = %d CPU result = %f Device result = %f\n";

//Some Library functions to be used.
template <typename T>
struct aligned_allocator
{
  using value_type = T;
  T* allocate(std::size_t num)
  {
    void* ptr = nullptr;
    if (posix_memalign(&ptr,4096,num*sizeof(T)))
      throw std::bad_alloc();
    return reinterpret_cast<T*>(ptr);
  }
  void deallocate(T* p, std::size_t num)
  {
    free(p);
  }
};


#define OCL_CHECK(error,call)                                       \
    call;                                                           \
    if (error != CL_SUCCESS) {                                      \
      printf("%s:%d Error calling " #call ", error code is: %d\n",  \
              __FILE__,__LINE__, error);                            \
      exit(EXIT_FAILURE);                                           \
    }                                       
	
namespace xcl {
	std::vector<cl::Device> get_devices(const std::string& vendor_name) {
		size_t i;
		cl_int err;
		std::vector<cl::Platform> platforms;
		OCL_CHECK(err, err = cl::Platform::get(&platforms));
		cl::Platform platform;
		for (i  = 0 ; i < platforms.size(); i++){
			platform = platforms[i];
			OCL_CHECK(err, std::string platformName = platform.getInfo<CL_PLATFORM_NAME>(&err));
			if (platformName == vendor_name){
				std::cout << "Found Platform" << std::endl;
				std::cout << "Platform Name: " << platformName.c_str() << std::endl;
				break;
			}
		}
		if (i == platforms.size()) {
			std::cout << "Error: Failed to find Xilinx platform" << std::endl;
			exit(EXIT_FAILURE);
		}
	   
		//Getting ACCELERATOR Devices and selecting 1st such device 
		std::vector<cl::Device> devices;
		OCL_CHECK(err, err = platform.getDevices(CL_DEVICE_TYPE_ACCELERATOR, &devices));
		return devices;
	}
	   
	std::vector<cl::Device> get_xil_devices() {
		return get_devices("Xilinx");
	}

	char* read_binary_file(const std::string &xclbin_file_name, unsigned &nb) 
	{
		std::cout << "INFO: Reading " << xclbin_file_name << std::endl;

		if(access(xclbin_file_name.c_str(), R_OK) != 0) {
			printf("ERROR: %s xclbin not available please build\n", xclbin_file_name.c_str());
			exit(EXIT_FAILURE);
		}
		//Loading XCL Bin into char buffer 
		std::cout << "Loading: '" << xclbin_file_name.c_str() << "'\n";
		std::ifstream bin_file(xclbin_file_name.c_str(), std::ifstream::binary);
		bin_file.seekg (0, bin_file.end);
		nb = bin_file.tellg();
		bin_file.seekg (0, bin_file.beg);
		char *buf = new char [nb];
		bin_file.read(buf, nb);
		return buf;
	}
};

void sgd_CPU(
	float* _params,
	half* grads,
	float* _exp_avg,
	size_t _param_size)
{
#define TILE 256
    size_t rounded_size = 0;
    if (_param_size > rounded_size) {

        for (size_t t = rounded_size; t < _param_size; t += TILE) {
            size_t copy_size = TILE;
            if ((t + TILE) > _param_size) copy_size = _param_size - t;
            size_t offset = copy_size + t;
#pragma omp parallel for
            for (size_t k = t; k < offset; k++) {
				float grad = _cvtsh_ss(grads[k]);
				grad *= scale;

                float param = _params[k];
                float momentum = _exp_avg[k];
				
                grad = param * w_decay + grad;
                momentum = grad * _betta_minus1 + momentum * _betta;

                param = momentum * step_size + param;
                _params[k] = param;
                _exp_avg[k] = momentum;
            }
        }

    }

}

int sgd_fpga(size_t nbytes, cl::Context context, cl::CommandQueue q, cl::Kernel krnl_adam){
	int err;

	int nvmeFd_param = -1;
	int nvmeFd_grad = -1;
	int nvmeFd_exp_avg = -1;
	int ret;
    
	std::chrono::high_resolution_clock::time_point prepare_start = std::chrono::high_resolution_clock::now();

	nvmeFd_param = open(param_name.c_str(), O_RDWR | O_SYNC | O_CREAT | O_DIRECT, 0644);
	nvmeFd_grad = open(grad_name.c_str(), O_RDWR | O_SYNC | O_CREAT | O_DIRECT, 0644);
	nvmeFd_exp_avg = open(exp_avg_name.c_str(), O_RDWR | O_SYNC | O_CREAT | O_DIRECT, 0644);
	
	cl_mem_ext_ptr_t outExt = {0};
	outExt.flags = XCL_MEM_EXT_P2P_BUFFER;

	cl::Buffer param(context, CL_MEM_READ_WRITE | CL_MEM_EXT_PTR_XILINX, nbytes, &outExt, nullptr);
	
	cl::Buffer grad(context, CL_MEM_READ_WRITE | CL_MEM_EXT_PTR_XILINX, nbytes/2, &outExt, nullptr);
	
	std::vector<half, aligned_allocator<half>> param16_ptr(nbytes/sizeof(float));

	cl::Buffer param16(context, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, nbytes/2, (void*) &param16_ptr[0], nullptr);
	
	cl::Buffer exp_avg(context, CL_MEM_READ_WRITE | CL_MEM_EXT_PTR_XILINX, nbytes, &outExt, nullptr);
	
		
	unsigned int cnt = 0;
    OCL_CHECK(err, err = krnl_adam.setArg(cnt++, grad));
	OCL_CHECK(err, err = krnl_adam.setArg(cnt++, param16));
	OCL_CHECK(err, err = krnl_adam.setArg(cnt++, param));
    OCL_CHECK(err, err = krnl_adam.setArg(cnt++, exp_avg));
	OCL_CHECK(err, err = krnl_adam.setArg(cnt++, DATA_SIZE));
	
	OCL_CHECK(err, err = krnl_adam.setArg(cnt++, _betta));
	OCL_CHECK(err, err = krnl_adam.setArg(cnt++, w_decay));
	OCL_CHECK(err, err = krnl_adam.setArg(cnt++, step_size));
	OCL_CHECK(err, err = krnl_adam.setArg(cnt++, scale));
	
	std::vector<cl::Event> events;
	cl::Event in1_event, in2_event, in3_event, in4_event;
	size_t offset = 0;	

	float* p2p_param = (float*)q.enqueueMapBuffer(param,						// buffer
									  CL_TRUE,						// blocking call
									  CL_MAP_WRITE | CL_MAP_READ,	//Indicates we will write
									  0,							// buffer offset
									  nbytes,			// size in bytes
									  nullptr, nullptr,
									  &err);
	half* p2p_grad = (half*)q.enqueueMapBuffer(grad,						// buffer
									  CL_TRUE,						// blocking call
									  CL_MAP_WRITE | CL_MAP_READ,	//Indicates we will write
									  0,							// buffer offset
									  nbytes/2,			// size in bytes
									  nullptr, nullptr,
									  &err);
	float* p2p_exp_avg = (float*)q.enqueueMapBuffer(exp_avg,						// buffer
									  CL_TRUE,						// blocking call
									  CL_MAP_WRITE | CL_MAP_READ,	//Indicates we will write
									  0,							// buffer offset
									  nbytes,			// size in bytes
									  nullptr, nullptr,
									  &err);
	
	OCL_CHECK(err, err =q.finish());
	std::chrono::high_resolution_clock::time_point prepare_end = std::chrono::high_resolution_clock::now();
    cl_ulong prepare_time = std::chrono::duration_cast<std::chrono::microseconds>(prepare_end - prepare_start).count();
    double dnsduration = (double)prepare_time;
	std::cout << "Prepare time = " << dnsduration << std::setprecision(2)<< std::fixed  << std::endl;

	//double dsduration = dnsduration / ((double)1000000);
	//double gbpersec = (iter * bufsize / dsduration) / ((double)1024 * 1024 * 1024);

	std::chrono::high_resolution_clock::time_point total_start = std::chrono::high_resolution_clock::now();
	std::chrono::high_resolution_clock::time_point p2p_start = std::chrono::high_resolution_clock::now();
	ret = pread(nvmeFd_param, (void*)p2p_param, nbytes, 0);
	if (ret == -1) {
		std::cout << "P2P: read() failed, err: " << ret << ", line: " << __LINE__ << std::endl;
		return EXIT_FAILURE;
	}
	ret = pread(nvmeFd_grad, (void*)p2p_grad, nbytes/2, 0);
	if (ret == -1) {
		std::cout << "P2P: read() failed, err: " << ret << ", line: " << __LINE__ << std::endl;
		return EXIT_FAILURE;
	}
	ret = pread(nvmeFd_exp_avg, (void*)p2p_exp_avg, nbytes, 0);
	if (ret == -1) {
		std::cout << "P2P: read() failed, err: " << ret << ", line: " << __LINE__ << std::endl;
		return EXIT_FAILURE;
	}
	std::chrono::high_resolution_clock::time_point p2p_end = std::chrono::high_resolution_clock::now();
    cl_ulong p2p_time = std::chrono::duration_cast<std::chrono::microseconds>(p2p_end - p2p_start).count();
    dnsduration = (double)p2p_time;
	double dsduration = dnsduration / ((double)1000000);
	double gbpersec = (4*nbytes / dsduration) / ((double)1024 * 1024 * 1024);
	std::cout << "pread : Buffer = " << 4*nbytes << " Throughput = " << std::setprecision(2)
			  << std::fixed << gbpersec << "GB/s\n";


	std::chrono::high_resolution_clock::time_point compute_start = std::chrono::high_resolution_clock::now();
    //set the kernel Arguments
	

    //Launch the Kernel
	cl::Event run_event;	
    q.enqueueTask(krnl_adam, &events, &run_event);
	events.push_back(run_event);

    q.finish();
	std::chrono::high_resolution_clock::time_point compute_end = std::chrono::high_resolution_clock::now();
	cl_ulong compute_time = std::chrono::duration_cast<std::chrono::microseconds>(compute_end - compute_start).count();
    dnsduration = (double)compute_time;
	dsduration = dnsduration / ((double)1000000);
	gbpersec = (2.5 * nbytes / dsduration) / ((double)1024 * 1024 * 1024);
	std::cout << "Compute : Buffer = " << 2.5 * nbytes << " Throughput = " << std::setprecision(2)
			  << std::fixed << gbpersec << "GB/s\n";

	
	std::chrono::high_resolution_clock::time_point pwrite_start = std::chrono::high_resolution_clock::now();
	
	ret = pwrite(nvmeFd_param, (void*)p2p_param, nbytes, 0);
	if (ret == -1) {
		std::cout << "P2P: write() failed, err: " << ret << ", line: " << __LINE__ << std::endl;
		return EXIT_FAILURE;
	}
	ret = pwrite(nvmeFd_exp_avg, (void*)p2p_exp_avg, nbytes, 0);
	if (ret == -1) {
		std::cout << "P2P: write() failed, err: " << ret << ", line: " << __LINE__ << std::endl;
		return EXIT_FAILURE;
	}
    q.finish();
	std::chrono::high_resolution_clock::time_point pwrite_end = std::chrono::high_resolution_clock::now();
	
	cl_ulong pwrite_time = std::chrono::duration_cast<std::chrono::microseconds>(pwrite_end - pwrite_start).count();
    dnsduration = (double)pwrite_time;
	dsduration = dnsduration / ((double)1000000);
	gbpersec = (3 * nbytes / dsduration) / ((double)1024 * 1024 * 1024);
	std::cout << "pwrite : Buffer = " << 3 << " Throughput = " << std::setprecision(2)
			  << std::fixed << gbpersec << "GB/s\n";

	std::chrono::high_resolution_clock::time_point total_end = std::chrono::high_resolution_clock::now();
    cl_ulong total_time = std::chrono::duration_cast<std::chrono::microseconds>(total_end - total_start).count();
    dnsduration = (double)total_time;
	dsduration = dnsduration / ((double)1000000);
	gbpersec = (4*nbytes / dsduration) / ((double)1024 * 1024 * 1024);
	std::cout << "Total : Buffer = " << 4 << " Throughput = " << std::setprecision(2)
			  << std::fixed << gbpersec << "GB/s\n";

	(void)close(nvmeFd_param);
	(void)close(nvmeFd_grad);
	(void)close(nvmeFd_exp_avg);
	
	return 0;
}


void print_device_bdf(const std::vector<cl::Device>& devices) {
    char device_bdf[20];
    cl_int err;
    cl::Device device;
    for (uint32_t i = 0; i < devices.size(); i++) {
        OCL_CHECK(err, err = devices[i].getInfo(CL_DEVICE_PCIE_BDF, &device_bdf));
		std::cout << device_bdf << std::endl;
    }
}

int main(int argc, char* argv[]) {

	cl_int err;
	const char* xclbinFilename = "sgd.xclbin";
	//const char* xclbinFilename = argv[1];

    // Creates a vector of DATA_SIZE elements with an initial value of 10 and 32
    // using customized allocator for getting buffer alignment to 4k boundary
	
	std::vector<cl::Device> devices = xcl::get_xil_devices();
	//print_device_bdf (devices);
    cl::Device device = devices[0];
	
    devices.resize(1);
	
    // Creating Context and Command Queue for selected device
	cl::Context* ctx_ptr = new cl::Context(devices[0]);
	cl::Context context = *ctx_ptr;
    cl::CommandQueue q = cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE);

    // Load xclbin 
    std::cout << "Loading: '" << xclbinFilename << "'\n";
    std::ifstream bin_file(xclbinFilename, std::ifstream::binary);
    bin_file.seekg (0, bin_file.end);
    unsigned nb = bin_file.tellg(); // print out #bits of file
    bin_file.seekg (0, bin_file.beg);
    char *buf = new char [nb];
    bin_file.read(buf, nb);
	bin_file.close();

    // Creating Program from Binary File
    cl::Program::Binaries bins;
    bins.push_back({buf,nb}); //Multiple bin files is possible

	cl::Program program = cl::Program(context, devices, bins);

    cl::Kernel krnl_sgd(program,"krnl_vadd");
	delete[] buf;

    std::cout << "Loading Success!: '" << xclbinFilename << "'\n";

	int nvmeFd = -1;
	int ret;
	nvmeFd = open(param_name.c_str(), O_RDWR | O_SYNC | O_CREAT | O_DIRECT, 0644);
	if (nvmeFd < 0){
		std::cerr << "ERROR: open " << param_name << " failed with " << nvmeFd << std::endl;
		return EXIT_FAILURE;
	}

	struct stat fstat;
	stat(param_name.c_str(), &fstat);
	int blksize = 128;
	std::cout << "blksize : " << blksize<<std::endl;
	assert(blksize == 128);

	size_t padded_size = ((DATA_SIZE-1)/blksize + 1 ) * blksize;
	size_t nbytes = padded_size * sizeof(float);

	std::vector<float, aligned_allocator<float>> param_src(padded_size);
	std::vector<half, aligned_allocator<half>> grad_src(padded_size);
	std::vector<float, aligned_allocator<float>> exp_avg_src(padded_size, 0.);

	float rand_scale = 8;
	float rand_max = float(RAND_MAX);

	for (unsigned int i=0; i< DATA_SIZE; i++) {
		float grad32 = static_cast<float>((rand() - rand_max / 2) / rand_max) * rand_scale;
		grad_src[i] = _cvtss_sh(grad32, 0);
		float ref_val= _cvtsh_ss(grad_src[i]);

		grad_src[i] = _cvtss_sh(ref_val, 0);
		ref_val= _cvtsh_ss(grad_src[i]);

		param_src[i] = static_cast<float>((rand() - rand_max / 2) / rand_max) * rand_scale;
		exp_avg_src[i] = static_cast<float>((rand() - rand_max / 2) / rand_max) * rand_scale;
	}
	

	ret = pwrite(nvmeFd,  &param_src[0], nbytes, 0);
	if (ret == -1) {
		std::cout << strerror(errno) << std::endl;
		std::cout << "normal pwrite() failed, err: " << ret << ", line: " << __LINE__ << std::endl;
		return EXIT_FAILURE;
	}
	(void)close(nvmeFd);

	nvmeFd = open(grad_name.c_str(), O_RDWR | O_SYNC | O_CREAT | O_DIRECT, 0644);
	if (nvmeFd < 0){
		std::cerr << "ERROR: open " << grad_name << " failed with " << nvmeFd << std::endl;
		return EXIT_FAILURE;
	}
	ret = pwrite(nvmeFd,  &grad_src[0], nbytes/2, 0);
	if (ret == -1) {
		std::cout << strerror(errno) << std::endl;
		std::cout << "normal pwrite() failed, err: " << ret << ", line: " << __LINE__ << std::endl;
		return EXIT_FAILURE;
	}
	(void)close(nvmeFd);
	
	nvmeFd = open(exp_avg_name.c_str(), O_RDWR | O_SYNC | O_CREAT | O_DIRECT, 0644);
	if (nvmeFd < 0){
		std::cerr << "ERROR: open " << exp_avg_name << " failed with " << nvmeFd << std::endl;
		return EXIT_FAILURE;
	}
	ret = pwrite(nvmeFd,  &exp_avg_src[0], nbytes, 0);
	if (ret == -1) {
		std::cout << strerror(errno) << std::endl;
		std::cout << "normal pwrite() failed, err: " << ret << ", line: " << __LINE__ << std::endl;
		return EXIT_FAILURE;
	}
	(void)close(nvmeFd);

	ret = sgd_fpga(nbytes, context, q, krnl_sgd);

	if (ret == EXIT_FAILURE){
		std::cout<< "FPGA adam failed ... " << std::endl;
		return EXIT_FAILURE;
	}

	std::vector<float, aligned_allocator<float>> param_dst(padded_size);
	std::vector<float, aligned_allocator<float>> exp_avg_dst(padded_size, 0.);

	// Reference value
	std::vector<float, aligned_allocator<float>> param_ref(padded_size);
	std::vector<float, aligned_allocator<float>> exp_avg_ref(padded_size, 0.);

	memcpy(&param_ref[0], &param_src[0], nbytes);
	memcpy(&exp_avg_ref[0], &exp_avg_src[0], nbytes);
	
	//Verify the result
    int match = 0;
	sgd_CPU( &param_ref[0], &grad_src[0], &exp_avg_ref[0], DATA_SIZE );

	nvmeFd = open(param_name.c_str(), O_RDWR | O_SYNC | O_CREAT | O_DIRECT, 0644);
	if (nvmeFd < 0){
		std::cerr << "ERROR: open " << param_name << " failed with " << nvmeFd << std::endl;
		return EXIT_FAILURE;
	}
	ret = pread(nvmeFd,  &param_dst[0], nbytes, 0);
	if (ret == -1) {
		std::cout << strerror(errno) << std::endl;
		std::cout << "normal pwrite() failed, err: " << ret << ", line: " << __LINE__ << std::endl;
		return EXIT_FAILURE;
	}
	(void)close(nvmeFd);

	nvmeFd = open(exp_avg_name.c_str(), O_RDWR | O_SYNC | O_CREAT | O_DIRECT, 0644);
	if (nvmeFd < 0){
		std::cerr << "ERROR: open " << exp_avg_name << " failed with " << nvmeFd << std::endl;
		return EXIT_FAILURE;
	}
	ret = pread(nvmeFd,  &exp_avg_dst[0], nbytes, 0);
	if (ret == -1) {
		std::cout << strerror(errno) << std::endl;
		std::cout << "normal pwrite() failed, err: " << ret << ", line: " << __LINE__ << std::endl;
		return EXIT_FAILURE;
	}
	(void)close(nvmeFd);



	float eps =  1e-6;
	int cnt = 0;
	std::cout << std::setprecision(8) <<std::fixed;
    for (int i = 0; i < DATA_SIZE; i++) {
		//std::cout << std::abs(param_dst[i] - param_ref[i]) << std::endl;
		match = 0;
        if (std::abs(param_dst[i] - param_ref[i]) > eps) {
			std::cout << "["<< i << "] param failed ori: "<< param_src[i]<<", ref: " << param_ref[i] <<", device: "<< param_dst[i]<<std::endl;
            match = 1;
        }
		if (std::abs(exp_avg_dst[i] - exp_avg_ref[i]) > eps) {
			std::cout <<"["<< i << "] exp_avg failed ori: "<< exp_avg_src[i] <<", ref:" <<exp_avg_ref[i] <<", device: "<< exp_avg_dst[i]<<std::endl;
            match = 1;
        }
    }

	std::cout << "cnt = "<< cnt << std::endl;
    std::cout << "TEST WITH ONE KERNEL " << (match ? "FAILED" : "PASSED") << std::endl; 

    return (match ? EXIT_FAILURE :  EXIT_SUCCESS);


	return 0;
}
