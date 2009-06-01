/*
 *  Copyright 2008-2009 NVIDIA Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */



#pragma once

#include <cusp/coo_matrix.h>
#include <cusp/memory.h>
#include <cusp/device/common.h>
#include <cusp/device/utils.h>
#include <cusp/device/texture.h>
#include <cusp/device/spmv_coo_serial.h>

namespace cusp
{

namespace device
{


// segmented reduction in shared memory
template <typename IndexType, typename ValueType>
__device__ void segreduce_warp(const IndexType * idx, ValueType * val)
{
    const IndexType thread_lane = threadIdx.x & (WARP_SIZE - 1);               // thread index within the warp

    if( thread_lane >=  1 && idx[threadIdx.x] == idx[threadIdx.x -  1] ) { val[threadIdx.x] += val[threadIdx.x -  1]; EMUSYNC; } 
    if( thread_lane >=  2 && idx[threadIdx.x] == idx[threadIdx.x -  2] ) { val[threadIdx.x] += val[threadIdx.x -  2]; EMUSYNC; }
    if( thread_lane >=  4 && idx[threadIdx.x] == idx[threadIdx.x -  4] ) { val[threadIdx.x] += val[threadIdx.x -  4]; EMUSYNC; }
    if( thread_lane >=  8 && idx[threadIdx.x] == idx[threadIdx.x -  8] ) { val[threadIdx.x] += val[threadIdx.x -  8]; EMUSYNC; }
    if( thread_lane >= 16 && idx[threadIdx.x] == idx[threadIdx.x - 16] ) { val[threadIdx.x] += val[threadIdx.x - 16]; EMUSYNC; }
}
template <typename IndexType, typename ValueType>
__device__ void segreduce_block(const IndexType * idx, ValueType * val)
{
    // TODO use segreduce_warp
    
    ValueType left = 0;
    if( threadIdx.x >=   1 && idx[threadIdx.x] == idx[threadIdx.x -   1] ) { left = val[threadIdx.x -   1]; } __syncthreads(); val[threadIdx.x] += left; left = 0; __syncthreads();  
    if( threadIdx.x >=   2 && idx[threadIdx.x] == idx[threadIdx.x -   2] ) { left = val[threadIdx.x -   2]; } __syncthreads(); val[threadIdx.x] += left; left = 0; __syncthreads();
    if( threadIdx.x >=   4 && idx[threadIdx.x] == idx[threadIdx.x -   4] ) { left = val[threadIdx.x -   4]; } __syncthreads(); val[threadIdx.x] += left; left = 0; __syncthreads();
    if( threadIdx.x >=   8 && idx[threadIdx.x] == idx[threadIdx.x -   8] ) { left = val[threadIdx.x -   8]; } __syncthreads(); val[threadIdx.x] += left; left = 0; __syncthreads();
    if( threadIdx.x >=  16 && idx[threadIdx.x] == idx[threadIdx.x -  16] ) { left = val[threadIdx.x -  16]; } __syncthreads(); val[threadIdx.x] += left; left = 0; __syncthreads();
    if( threadIdx.x >=  32 && idx[threadIdx.x] == idx[threadIdx.x -  32] ) { left = val[threadIdx.x -  32]; } __syncthreads(); val[threadIdx.x] += left; left = 0; __syncthreads();  
    if( threadIdx.x >=  64 && idx[threadIdx.x] == idx[threadIdx.x -  64] ) { left = val[threadIdx.x -  64]; } __syncthreads(); val[threadIdx.x] += left; left = 0; __syncthreads();
    if( threadIdx.x >= 128 && idx[threadIdx.x] == idx[threadIdx.x - 128] ) { left = val[threadIdx.x - 128]; } __syncthreads(); val[threadIdx.x] += left; left = 0; __syncthreads();
    if( threadIdx.x >= 256 && idx[threadIdx.x] == idx[threadIdx.x - 256] ) { left = val[threadIdx.x - 256]; } __syncthreads(); val[threadIdx.x] += left; left = 0; __syncthreads();
}

//////////////////////////////////////////////////////////////////////////////
// COO SpMV kernel which flattens data irregularity (segmented reduction)
//////////////////////////////////////////////////////////////////////////////
//
// spmv_coo_flat
//   The input coo_matrix must be sorted by row.  Columns within each row
//   may appear in any order and duplicate entries are also acceptable.
//   This sorted COO format is easily obtained by expanding the row pointer
//   of a CSR matrix (csr.Ap) into proper row indices and then copying 
//   the arrays containing the CSR column indices (csr.Aj) and nonzero values
//   (csr.Ax) verbatim.  A segmented reduction is used to compute the per-row
//   sums.
//
// spmv_coo_flat_tex
//   Same as spmv_coo_flat, except that the texture cache is 
//   used for accessing the x vector.
//
// spmv_coo_flat_atomic
//   Similar to spmv_coo_flat except atomic memory operations are
//   used to avoid using a temporary buffer to store the second level
//   results of the segmented reduction.
//
// spmv_coo_flat_atomic_tex
//   Same as spmv_coo_flat_atomic, except that the texture cache is 
//   used for accessing the x vector.

template <typename IndexType, typename ValueType, unsigned int BLOCK_SIZE, bool UseCache>
__global__ void
spmv_coo_flat_kernel(const IndexType num_entries,
                     const IndexType interval_size,
                     const IndexType * I, 
                     const IndexType * J, 
                     const ValueType * V, 
                     const ValueType * x, 
                           ValueType * y,
                           IndexType * temp_rows,
                           ValueType * temp_vals)
{
    __shared__ IndexType idx[BLOCK_SIZE];
    __shared__ ValueType val[BLOCK_SIZE];
    __shared__ IndexType carry_idx[BLOCK_SIZE / 32];
    __shared__ ValueType carry_val[BLOCK_SIZE / 32];

    const IndexType thread_id   = BLOCK_SIZE * blockIdx.x + threadIdx.x;     // global thread index
    const IndexType thread_lane = threadIdx.x & (WARP_SIZE-1);               // thread index within the warp
    const IndexType warp_id     = thread_id   / WARP_SIZE;                   // global warp index
    const IndexType warp_lane   = threadIdx.x / WARP_SIZE;                   // warp index within the CTA

    const IndexType begin = warp_id * interval_size + thread_lane;           // thread's offset into I,J,V
    const IndexType end   = min(begin + interval_size, num_entries);        // end of thread's work

    if(begin >= end){ return; }                                              // warp has no work to do 

    if (thread_lane == 0){
        // initialize the carry in values
        carry_idx[warp_lane] = I[begin]; 
        carry_val[warp_lane] = 0;
    }
    
    for(IndexType n = begin; n < end; n += WARP_SIZE){
        idx[threadIdx.x] = I[n];                                             // row index
        val[threadIdx.x] = V[n] * fetch_x<UseCache>(J[n], x);                // val = A[row,col] * x[col] 

        if (thread_lane == 0){
            if(idx[threadIdx.x] == carry_idx[warp_lane])
                val[threadIdx.x] += carry_val[warp_lane];                    // row continues into this warp's span
            else
                y[carry_idx[warp_lane]] += carry_val[warp_lane];             // row terminated
        }

        // segmented reduction in shared memory
        segreduce_warp(idx, val);

        if( thread_lane == 31 ) {
            carry_idx[warp_lane] = idx[threadIdx.x];                         // last thread in warp saves its results
            carry_val[warp_lane] = val[threadIdx.x];
        }
        else if ( idx[threadIdx.x] != idx[threadIdx.x+1] ) {
            y[idx[threadIdx.x]] += val[threadIdx.x];                         // row terminated
        }
        
    }

    // final carry
    if(thread_lane == 31){
        temp_rows[warp_id] = carry_idx[warp_lane];
        temp_vals[warp_id] = carry_val[warp_lane];
    }
}

// The second level of the segmented reduction operation (default)
template <typename IndexType, typename ValueType, unsigned int BLOCK_SIZE>
__global__ void
spmv_coo_reduce_update_kernel(const IndexType num_warps,
                              const IndexType * temp_rows,
                              const ValueType * temp_vals,
                                    ValueType * y)
{
//// FAILSAFE
//    if (threadIdx.x == 0)
//        for(IndexType n = 0; n < num_warps; n++)
//            y[temp_rows[n]] += temp_vals[n];


    __shared__ IndexType rows[BLOCK_SIZE + 1];    
    __shared__ ValueType vals[BLOCK_SIZE + 1];    

    const IndexType end = num_warps - (num_warps & (BLOCK_SIZE - 1));

    if (threadIdx.x == 0){
        rows[BLOCK_SIZE] = (IndexType) -1;
        vals[BLOCK_SIZE] = (ValueType)  0;
    }
    
    __syncthreads();

    IndexType i = threadIdx.x;

    while (i < end){
        // do full blocks
        rows[threadIdx.x] = temp_rows[i];
        vals[threadIdx.x] = temp_vals[i];

        __syncthreads();

        segreduce_block(rows, vals);

        if (rows[threadIdx.x] != rows[threadIdx.x + 1])
            y[rows[threadIdx.x]] += vals[threadIdx.x];

        __syncthreads();

        i += BLOCK_SIZE; 
    }

    if (end < num_warps){
        if (i < num_warps){
            rows[threadIdx.x] = temp_rows[i];
            vals[threadIdx.x] = temp_vals[i];
        } else {
            rows[threadIdx.x] = (IndexType) -1;
            vals[threadIdx.x] = (ValueType)  0;
        }

        __syncthreads();
   
        segreduce_block(rows, vals);

        if (i < num_warps)
            if (rows[threadIdx.x] != rows[threadIdx.x + 1])
                y[rows[threadIdx.x]] += vals[threadIdx.x];
    }
}

// The second level of the segmented reduction operation (alternative)
template <typename IndexType, typename ValueType, unsigned int BLOCK_SIZE>
__global__ void
spmv_coo_scatter_update_kernel(const IndexType num_warps,
                               const IndexType * temp_rows,
                               const ValueType * temp_vals,
                                     ValueType * y)
{
    for(IndexType i = threadIdx.x; i < num_warps; i += BLOCK_SIZE)
        y[temp_rows[i]] += temp_vals[i];
}


// Atomic version of the COO kernel
template <typename IndexType, typename ValueType, unsigned int BLOCK_SIZE, bool UseCache>
__global__ void
spmv_coo_flat_atomic_kernel(const IndexType num_entries,
                            const IndexType interval_size,
                            const IndexType * I, 
                            const IndexType * J, 
                            const ValueType * V, 
                            const ValueType * x, 
                                  ValueType * y)
{
    __shared__ IndexType idx[BLOCK_SIZE];
    __shared__ ValueType val[BLOCK_SIZE];
    __shared__ IndexType carry_idx[BLOCK_SIZE / 32];
    __shared__ ValueType carry_val[BLOCK_SIZE / 32];

    const IndexType thread_id   = BLOCK_SIZE * blockIdx.x + threadIdx.x;     // global thread index
    const IndexType thread_lane = threadIdx.x & (WARP_SIZE-1);               // thread index within the warp
    const IndexType warp_id     = thread_id   / WARP_SIZE;                   // global warp index
    const IndexType warp_lane   = threadIdx.x / WARP_SIZE;                   // warp index within the CTA

    const IndexType begin = warp_id * interval_size + thread_lane;           // thread's offset into I,J,V
    const IndexType end   = min(begin + interval_size, num_entries);        // end of thread's work

    if(begin >= end) return;                                                 // warp has no work to do

    const IndexType first_idx = I[warp_id * interval_size];                  // first row of this warp's interval

    if (thread_lane == 0){
        carry_idx[warp_lane] = first_idx; 
        carry_val[warp_lane] = 0;
    }
    
    for(IndexType n = begin; n < end; n += WARP_SIZE){
        idx[threadIdx.x] = I[n];                                             // row index
        val[threadIdx.x] = V[n] * fetch_x<UseCache>(J[n], x);                // val = A[row,col] * x[col] 

        if (thread_lane == 0){
            if(idx[threadIdx.x] == carry_idx[warp_lane])
                val[threadIdx.x] += carry_val[warp_lane];                    // row continues into this warp's span
            else if(carry_idx[warp_lane] != first_idx)
                y[carry_idx[warp_lane]] += carry_val[warp_lane];             // row terminated, does not span boundary
            else
                atomicAdd(y + carry_idx[warp_lane], carry_val[warp_lane]);   // row terminated, but spans iter-warp boundary
        }

        // segmented scan in shared memory
        segreduce_warp(idx, val);

        if( thread_lane == 31 ) {
            carry_idx[warp_lane] = idx[threadIdx.x];                         // last thread in warp saves its results
            carry_val[warp_lane] = val[threadIdx.x];
        }
        else if ( idx[threadIdx.x] != idx[threadIdx.x+1] ) {                 // row terminates here
            if(idx[threadIdx.x] != first_idx)
                y[idx[threadIdx.x]] += val[threadIdx.x];                     // row terminated, does not span inter-warp boundary
            else
                atomicAdd(y + idx[threadIdx.x], val[threadIdx.x]);           // row terminated, but spans iter-warp boundary
        }
        
    }

    // final carry
    if(thread_lane == 31){
        atomicAdd(y + carry_idx[warp_lane], carry_val[warp_lane]); 
    }
}






template <typename IndexType, typename ValueType, bool UseCache>
void __spmv_coo_flat_atomic(const coo_matrix<IndexType,ValueType,cusp::device_memory>& d_coo, 
                            const ValueType * d_x, 
                                  ValueType * d_y)
{
    if(d_coo.num_entries == 0)
        return; //empty matrix
    else if (d_coo.num_entries < WARP_SIZE){
        spmv_coo_serial_kernel<IndexType,ValueType> <<<1,1>>>
            (d_coo.num_entries, d_coo.row_indices, d_coo.column_indices, d_coo.values, d_x, d_y);
        return;
    }

    const unsigned int BLOCK_SIZE      = 128;
    const unsigned int MAX_BLOCKS      = 4*MAX_THREADS / BLOCK_SIZE; // empirically  better on test cases
    const unsigned int WARPS_PER_BLOCK = BLOCK_SIZE / WARP_SIZE;

    const unsigned int num_units  = d_coo.num_entries / WARP_SIZE; 
    const unsigned int num_warps  = std::min(num_units, WARPS_PER_BLOCK * MAX_BLOCKS);
    const unsigned int num_blocks = DIVIDE_INTO(num_warps, WARPS_PER_BLOCK);
    const unsigned int num_iters  = DIVIDE_INTO(num_units, num_warps);
    
    const unsigned int interval_size = WARP_SIZE * num_iters;

    const IndexType tail = num_units * WARP_SIZE; // do the last few nonzeros separately

    if (UseCache)
        bind_x(d_x);

    spmv_coo_flat_atomic_kernel<IndexType, ValueType, BLOCK_SIZE, UseCache> <<<num_blocks, BLOCK_SIZE>>>
        (tail, interval_size, d_coo.row_indices, d_coo.column_indices, d_coo.values, d_x, d_y);

    spmv_coo_serial_kernel<IndexType,ValueType> <<<1,1>>>
        (d_coo.num_entries - tail, d_coo.row_indices + tail, d_coo.column_indices + tail, d_coo.values + tail, d_x, d_y);

    if (UseCache)
        unbind_x(d_x);
}

template <typename IndexType, typename ValueType>
void spmv_coo_flat_atomic(const coo_matrix<IndexType,ValueType,cusp::device_memory>& d_coo, 
                          const ValueType * d_x, 
                                ValueType * d_y)
{ 
    __spmv_coo_flat_atomic<IndexType, ValueType, false>(d_coo, d_x, d_y);
}


template <typename IndexType, typename ValueType>
void spmv_coo_flat_atomic_tex(const coo_matrix<IndexType,ValueType,cusp::device_memory>& d_coo, 
                              const ValueType * d_x, 
                                    ValueType * d_y)
{ 
    __spmv_coo_flat_atomic<IndexType, ValueType, true>(d_coo, d_x, d_y);
}

template <typename IndexType, typename ValueType, bool UseCache>
void __spmv_coo_flat(const coo_matrix<IndexType,ValueType,cusp::device_memory>& d_coo, 
                     const ValueType * d_x, 
                           ValueType * d_y)
{
    if(d_coo.num_entries == 0)
        return; //empty matrix
    else if (d_coo.num_entries < WARP_SIZE){
        spmv_coo_serial_kernel<IndexType,ValueType> <<<1,1>>>
            (d_coo.num_entries, d_coo.row_indices, d_coo.column_indices, d_coo.values, d_x, d_y);
        return;
    }

    //TODO Determine optimal BLOCK_SIZE and MAX_BLOCKS
    const unsigned int BLOCK_SIZE      = 256;
    const unsigned int MAX_BLOCKS      = MAX_THREADS / (2*BLOCK_SIZE);
    const unsigned int WARPS_PER_BLOCK = BLOCK_SIZE / WARP_SIZE;

    const unsigned int num_units  = d_coo.num_entries / WARP_SIZE; 
    const unsigned int num_warps  = std::min(num_units, WARPS_PER_BLOCK * MAX_BLOCKS);
    const unsigned int num_blocks = DIVIDE_INTO(num_warps, WARPS_PER_BLOCK);
    const unsigned int num_iters  = DIVIDE_INTO(num_units, num_warps);
    
    const unsigned int interval_size = WARP_SIZE * num_iters;

    const IndexType tail = num_units * WARP_SIZE; // do the last few nonzeros separately

    const unsigned int active_warps = (interval_size == 0) ? 0 : DIVIDE_INTO(tail, interval_size);

    if (UseCache)
        bind_x(d_x);

    IndexType * temp_rows = new_device_array<IndexType>(active_warps);
    ValueType * temp_vals = new_device_array<ValueType>(active_warps);

    spmv_coo_flat_kernel<IndexType, ValueType, BLOCK_SIZE, UseCache> <<<num_blocks, BLOCK_SIZE>>>
        (tail, interval_size, d_coo.row_indices, d_coo.column_indices, d_coo.values, d_x, d_y, temp_rows, temp_vals);

    spmv_coo_serial_kernel<IndexType,ValueType> <<<1,1>>>
        (d_coo.num_entries - tail, d_coo.row_indices + tail, d_coo.column_indices + tail, d_coo.values + tail, d_x, d_y);

    const bool host_method = false;

    if (host_method) {
        IndexType * h_temp_rows = cusp::duplicate_array_to_host(temp_rows, active_warps);
        ValueType * h_temp_vals = cusp::duplicate_array_to_host(temp_vals, active_warps);
    
        unsigned int unique_rows = 0;
    
        for(unsigned int start = 1; start < active_warps; start++){
            if(h_temp_rows[unique_rows] == h_temp_rows[start]) {
                h_temp_vals[unique_rows] += h_temp_vals[start];
            }
            else {
                unique_rows++;
                h_temp_rows[unique_rows] = h_temp_rows[start];
                h_temp_vals[unique_rows] = h_temp_vals[start];
            }
        }
        unique_rows++;
    
        cusp::memcpy_to_device(temp_rows, h_temp_rows, unique_rows);
        cusp::memcpy_to_device(temp_vals, h_temp_vals, unique_rows);
    
        cusp::delete_host_array(h_temp_rows);
        cusp::delete_host_array(h_temp_vals);
    
        spmv_coo_scatter_update_kernel<IndexType, ValueType, 512> <<<1, 512>>> (unique_rows, temp_rows, temp_vals, d_y);
    } else {
        //printf("active_warps %d\n", active_warps);
        spmv_coo_reduce_update_kernel<IndexType, ValueType, 512> <<<1, 512>>> (active_warps, temp_rows, temp_vals, d_y);
    }

    cusp::delete_device_array(temp_rows);
    cusp::delete_device_array(temp_vals);


    if (UseCache)
        unbind_x(d_x);
}

template <typename IndexType, typename ValueType>
void spmv_coo_flat(const coo_matrix<IndexType,ValueType,cusp::device_memory>& d_coo, 
                   const ValueType * d_x, 
                         ValueType * d_y)
{ 
    __spmv_coo_flat<IndexType, ValueType, false>(d_coo, d_x, d_y);
}


template <typename IndexType, typename ValueType>
void spmv_coo_flat_tex(const coo_matrix<IndexType,ValueType,cusp::device_memory>& d_coo, 
                       const ValueType * d_x, 
                             ValueType * d_y)
{ 
    __spmv_coo_flat<IndexType, ValueType, true>(d_coo, d_x, d_y);
}


} // end namespace device

} // end namespace cusp
