// Copyright (c) Microsoft Corporation.
// SPDX-License-Identifier: Apache-2.0

// DeepSpeed Team

#include "cpu_adagrad.h"
#include <torch/extension.h>
#include <iostream>
#include <memory>
#include <type_traits>
#include <unordered_map>
#if defined(__ENABLE_CUDA__)
#include <cuda_runtime_api.h>
#include "cublas_v2.h"
#include "cuda.h"
#include "curand.h"
#include "custom_cuda_layers.h"
#endif

static std::unordered_map<int, std::shared_ptr<void>> s_optimizers;

// C++ interface
#define CPU 0

void Adagrad_Optimizer::Step_cpu(float* _params,
                               float* grads,
                               float* _exp_avg_sq,
                               size_t _param_size,
                               ds_half_precision_t* dev_params,
                               bool half_precision)
{
	int p_n = 0;
	
	for (size_t t = 0; t < _param_size; t += TILE) {
		size_t copy_size = TILE;
		if ((t + TILE) > _param_size) copy_size = _param_size - t;
		size_t offset = copy_size + t;
#pragma omp parallel for
		for (size_t k = t; k < offset; k++) {
			//half h_g = (half)grads[k];
			//float grad = (float)h_g;
			float grad = grads[k];
			
			float step_size = -1 * _alpha;
			
			
			float param = _params[k];
			float momentum = grad;
			float variance = _exp_avg_sq[k];

			if (t + k  <  p_n)
			{
				std::cout <<  "Before G: " << grad << std::endl;
				std::cout <<  "Before P: " << param << std::endl;
				std::cout <<  "Before V: " << variance << std::endl;
			}

			if (_weight_decay > 0) { grad = param * _weight_decay + grad; }

			variance += grad * grad;

			grad = sqrt(variance);
			grad += _eps;
			grad = momentum / grad;
			param = grad * step_size + param;
			
			//half h_p = (half) param;
			//_params[k] = (float) h_p;
			_params[k] = param;
			// STORE UPDATE TERM TO GRAD'S MEMORY
			//grads[k] = grad * step_size;
			_exp_avg_sq[k] = variance;
			
			if (t + k  <  p_n)
			{
				std::cout <<  "After P: " << param << std::endl;
				std::cout <<  "After V: " << variance << std::endl;
			}
		}
    }
}

void Adagrad_Optimizer::Step_1(float* _params,
                               float* grads,
                               float* _exp_avg_sq,
                               size_t _param_size,
                               ds_half_precision_t* dev_params,
                               bool half_precision)
{
    size_t rounded_size = 0;
#if defined(__AVX512__) or defined(__AVX256__)
    Step_AVX<1>(
        &rounded_size, _params, grads, _exp_avg_sq, _param_size, dev_params, half_precision);
#endif
    if (_param_size > rounded_size) {
        float step_size = -1 * _alpha;
        ds_half_precision_t* grads_cast_h;
        ds_half_precision_t* params_cast_h;
        if (half_precision) {
            grads_cast_h = reinterpret_cast<ds_half_precision_t*>(grads);
            params_cast_h = reinterpret_cast<ds_half_precision_t*>(_params);
        }
        for (size_t t = rounded_size; t < _param_size; t += TILE) {
            size_t copy_size = TILE;
            if ((t + TILE) > _param_size) copy_size = _param_size - t;
            size_t offset = copy_size + t;
#if defined(__ENABLE_CUDA__)
            if ((t / TILE) >= 2) { cudaStreamSynchronize(_streams[_buf_index]); }
#endif
#pragma omp parallel for
            for (size_t k = t; k < offset; k++) {
                float grad = half_precision ? (float)grads_cast_h[k] : grads[k];
                float param = half_precision ? (float)params_cast_h[k] : _params[k];
                float momentum = grads[k];
                float variance = _exp_avg_sq[k];
                if (_weight_decay > 0) { grad = param * _weight_decay + grad; }

                variance += grad * grad;

                grad = sqrt(variance);
                grad += _eps;
                grad = momentum / grad;
                param = grad * step_size + param;
#if defined(__ENABLE_CUDA__)
                if (dev_params) _doubled_buffer[_buf_index][k - t] = param;
#endif
                if (half_precision)
                    params_cast_h[k] = (ds_half_precision_t)param;
                else
                    _params[k] = param;
                // STORE UPDATE TERM TO GRAD'S MEMORY
                grads[k] = grad * step_size;
                _exp_avg_sq[k] = variance;
            }
#if defined(__ENABLE_CUDA__)
            if (dev_params) {
                launch_param_update(
                    _doubled_buffer[_buf_index], dev_params + t, (copy_size), _streams[_buf_index]);
                _buf_index = !_buf_index;
            }
#endif
        }
    }
}

void Adagrad_Optimizer::Step_4(float* _params,
                               float* grads,
                               float* _exp_avg_sq,
                               size_t _param_size,
                               ds_half_precision_t* dev_params,
                               bool half_precision)
{
    size_t rounded_size = 0;
#if defined(__AVX512__) or defined(__AVX256__)
    Step_AVX<4>(
        &rounded_size, _params, grads, _exp_avg_sq, _param_size, dev_params, half_precision);
#endif
    if (_param_size > rounded_size)
        Step_1((_params + rounded_size),
               (grads + rounded_size),
               (_exp_avg_sq + rounded_size),
               (_param_size - rounded_size),
               (dev_params != nullptr ? (dev_params + rounded_size) : dev_params),
               half_precision);
}

int create_adagrad_optimizer(int optimizer_id,
                             float alpha = 1e-2,
                             float eps = 1e-8,
                             float weight_decay = 0,
                             bool should_log = false)
{
    auto opt = std::make_shared<Adagrad_Optimizer>(alpha, eps, weight_decay);

    s_optimizers[optimizer_id] = opt;

    if (should_log) {
        std::string avx_type = "";
#if defined(__AVX512__)
        avx_type = "AVX512";
#else
#if defined(__AVX256__)
        avx_type = "AVX2";
#else
        avx_type = "scalar";
#endif
#endif

        printf("Adagrad Optimizer #%d is created with %s arithmetic capability.\n",
               optimizer_id,
               avx_type.c_str());
        printf("Config: alpha=%f, weight_decay=%f\n", alpha, weight_decay);
    }

    return 0;
}

void Adagrad_Optimizer::Step_8(float* _params,
                               float* grads,
                               float* _exp_avg_sq,
                               size_t _param_size,
                               ds_half_precision_t* dev_params,
                               bool half_precision)
{
    size_t rounded_size = 0;
#if defined(__AVX512__) or defined(__AVX256__)
    Step_AVX<8>(
        &rounded_size, _params, grads, _exp_avg_sq, _param_size, dev_params, half_precision);
#endif
    if (_param_size > rounded_size)
        Step_4((_params + rounded_size),
               (grads + rounded_size),
               (_exp_avg_sq + rounded_size),
               (_param_size - rounded_size),
               (dev_params != nullptr ? (dev_params + rounded_size) : dev_params),
               half_precision);
}

int ds_adagrad_step(int optimizer_id,
                    size_t step,
                    float lr,
                    float epsilon,
                    float weight_decay,
                    torch::Tensor& params,
                    torch::Tensor& grads,
                    torch::Tensor& exp_avg_sq)
{
    auto params_c = params.contiguous();
    auto grads_c = grads.contiguous();
    auto exp_avg_sq_c = exp_avg_sq.contiguous();

    float* params_ptr = (float*)params_c.data_ptr();
    float* grads_ptr = (float*)grads_c.data_ptr();
    float* exp_avg_sq_ptr = (float*)exp_avg_sq_c.data_ptr();

    std::shared_ptr<Adagrad_Optimizer> opt =
        std::static_pointer_cast<Adagrad_Optimizer>(s_optimizers[optimizer_id]);
    opt->IncrementStep(step);
    opt->update_state(lr, epsilon, weight_decay);

#if CPU == 0
	opt->Step_8(params_ptr, grads_ptr, exp_avg_sq_ptr, params_c.numel());
#else 
    opt->Step_cpu(params_ptr, grads_ptr, exp_avg_sq_ptr, params_c.numel());
#endif

#if defined(__ENABLE_CUDA__)
    opt->SynchronizeStreams();
#endif
    return 0;
}

int ds_adagrad_step_plus_copy(int optimizer_id,
                              size_t step,
                              float lr,
                              float epsilon,
                              float weight_decay,
                              torch::Tensor& params,
                              torch::Tensor& grads,
                              torch::Tensor& exp_avg_sq,
                              torch::Tensor& gpu_params)
{
#if defined(__ENABLE_CUDA__)
    auto params_c = params.contiguous();
    auto gpu_params_c = gpu_params.contiguous();
    auto exp_avg_sq_c = exp_avg_sq.contiguous();
    auto grads_c = grads.contiguous();

    float* params_ptr = (float*)params_c.data_ptr();
    float* grads_ptr = (float*)grads_c.data_ptr();
    ds_half_precision_t* gpu_params_ptr = (ds_half_precision_t*)gpu_params_c.data_ptr();
    float* exp_avg_sq_ptr = (float*)exp_avg_sq_c.data_ptr();

    std::shared_ptr<Adagrad_Optimizer> opt =
        std::static_pointer_cast<Adagrad_Optimizer>(s_optimizers[optimizer_id]);
    opt->IncrementStep(step);
    opt->update_state(lr, epsilon, weight_decay);
    opt->Step_8(params_ptr,
                grads_ptr,
                exp_avg_sq_ptr,
                params_c.numel(),
                gpu_params_ptr,
                (params.options().dtype() == at::kHalf));

    opt->SynchronizeStreams();
#else
    assert(false);
#endif
    return 0;
}

int destroy_adagrad_optimizer(int optimizer_id)
{
    s_optimizers.erase(optimizer_id);

    return 0;
}

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m)
{
    m.def("adagrad_update", &ds_adagrad_step, "DeepSpeed CPU Adagrad update (C++)");
    m.def("adagrad_update_copy",
          &ds_adagrad_step_plus_copy,
          "DeepSpeed CPU Adagrad update and param copy (C++)");
    m.def("create_adagrad", &create_adagrad_optimizer, "DeepSpeed CPU Adagrad (C++)");
    m.def("destroy_adagrad", &destroy_adagrad_optimizer, "DeepSpeed CPU Adagrad destroy (C++)");
}
