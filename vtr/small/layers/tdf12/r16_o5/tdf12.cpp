#include "global_defines.h"
#include "tdf12_impl_defines.h"
#include <stdbool.h>
#include <assert.h>

#include "tdf12_conv_stages.h"

//////////////////////////////////////////////////////////////
//  ACCUMULATION FUNCTIONS
//////////////////////////////////////////////////////////////

// Accumulation stage 1
// This is a pipelined tree accumulation stage
// It reduces 128 inputs to 32 outputs.
// The estimated latency is 11 cycles.
void tdf12_accum_1(
   data_t accum_in[128], 
   data_t accum_out[32]
) {
   uint16_t out_idx = 0;
   IL_LOOP: for (uint16_t i1 = 0; i1 < 4; i1++) {
      uint16_t i = i1 * 32;
      #pragma HLS pipeline
      data_t vals[32];
      #pragma HLS array_partition variable=vals complete
      // This loop will be automatically unrolled and ideally all 
      // iterations of it must be scheduled in the same cycle.
      WRPC_LOOP: for (uint16_t w = 0; w < 32; w++) {
         // Need this bounds check because input length is not necessarily
         // a multiple of words read per cycle.
         vals[w] = (i+w < 128) ? accum_in[i+w] : (data_t)0;
      }
      data_t sum0 = vals[31] + vals[30];
      data_t sum1 = vals[29] + vals[28];
      data_t sum2 = vals[27] + vals[26];
      data_t sum3 = vals[25] + vals[24];
      data_t sum4 = vals[23] + vals[22];
      data_t sum5 = vals[21] + vals[20];
      data_t sum6 = vals[19] + vals[18];
      data_t sum7 = vals[17] + vals[16];
      data_t sum8 = vals[15] + vals[14];
      data_t sum9 = vals[13] + vals[12];
      data_t sum10 = vals[11] + vals[10];
      data_t sum11 = vals[9] + vals[8];
      data_t sum12 = vals[7] + vals[6];
      data_t sum13 = vals[5] + vals[4];
      data_t sum14 = vals[3] + vals[2];
      data_t sum15 = vals[1] + vals[0];
      data_t sum16 = sum0 + sum1;
      data_t sum17 = sum2 + sum3;
      data_t sum18 = sum4 + sum5;
      data_t sum19 = sum6 + sum7;
      data_t sum20 = sum8 + sum9;
      data_t sum21 = sum10 + sum11;
      data_t sum22 = sum12 + sum13;
      data_t sum23 = sum14 + sum15;
      accum_out[out_idx+0] = sum23;
      accum_out[out_idx+1] = sum22;
      accum_out[out_idx+2] = sum21;
      accum_out[out_idx+3] = sum20;
      accum_out[out_idx+4] = sum19;
      accum_out[out_idx+5] = sum18;
      accum_out[out_idx+6] = sum17;
      accum_out[out_idx+7] = sum16;
      out_idx += 8;

   }
}



// Accumulation stage 2
// This is a pipelined tree accumulation stage
// It reduces 32 inputs to 4 outputs.
// The estimated latency is 12 cycles.
void tdf12_accum_2(
   data_t accum_in[32], 
   data_t accum_out[4]
) {
   uint16_t out_idx = 0;
   IL_LOOP: for (uint16_t i1 = 0; i1 < 2; i1++) {
      uint16_t i = i1 * 16;
      #pragma HLS pipeline
      data_t vals[16];
      #pragma HLS array_partition variable=vals complete
      // This loop will be automatically unrolled and ideally all 
      // iterations of it must be scheduled in the same cycle.
      WRPC_LOOP: for (uint16_t w = 0; w < 16; w++) {
         // Need this bounds check because input length is not necessarily
         // a multiple of words read per cycle.
         vals[w] = (i+w < 32) ? accum_in[i+w] : (data_t)0;
      }
      data_t sum0 = vals[15] + vals[14];
      data_t sum1 = vals[13] + vals[12];
      data_t sum2 = vals[11] + vals[10];
      data_t sum3 = vals[9] + vals[8];
      data_t sum4 = vals[7] + vals[6];
      data_t sum5 = vals[5] + vals[4];
      data_t sum6 = vals[3] + vals[2];
      data_t sum7 = vals[1] + vals[0];
      data_t sum8 = sum0 + sum1;
      data_t sum9 = sum2 + sum3;
      data_t sum10 = sum4 + sum5;
      data_t sum11 = sum6 + sum7;
      data_t sum12 = sum8 + sum9;
      data_t sum13 = sum10 + sum11;
      accum_out[out_idx+0] = sum13;
      accum_out[out_idx+1] = sum12;
      out_idx += 2;

   }
}



// Accumulation stage 3
// This is an unpipelined tree accumulation stage.
// It reduces 4 inputs to 1 output.
// The estimated latency is 5 cycles.
data_t tdf12_accum_3(data_t accum_in[4]) {
   data_t sum0 = accum_in[3] + accum_in[2];
   data_t sum1 = accum_in[1] + accum_in[0];
   data_t sum2 = sum0 + sum1;
   return sum2;

}



// Function that keeps track of indices i,j,k for the top loop
// i and j are the output row and column coordinates, respectively
// k represents the output channel, but not directly. It actually 
// represents the group of output channels, since we can parallelize
// mutliple output channels for the same output XY coordinate. 
// For example, if OCHAN_SCALE_FACTOR = 4 (meaning we process 4 output channels
// at the same time), then k = 1 represents output channels 4,5,6,7.
void tdf12_get_next_ijk (uint16_t indices[3]) {
   static uint16_t i = 0;
   static uint16_t j = 0;
   static uint16_t k = 0;
   indices[0] = i;
   indices[1] = j;
   indices[2] = k;
   k++;
   if (k == OUTPUT_CHANS / OCHAN_SCALE_FACTOR) {
      k = 0;
      j++;
      if (j == OUTPUT_WIDTH) {
         j = 0;
         i++;
         if (i == OUTPUT_HEIGHT) {
            i = 0;
         }
      }
   }
}


void tdf12 (
   data_t in_data[INPUT_HEIGHT][INPUT_WIDTH][INPUT_CHANS_PADDED],
   data_t out_data[OUTPUT_HEIGHT][OUTPUT_WIDTH][OUTPUT_CHANS],
   data_t filter_data[OUTPUT_CHANS][FILTER_SIZE][FILTER_SIZE][INPUT_CHANS],
   data_t adjustments[OUTPUT_CHANS][4]
) {
   // Ideally, this single for loop would be split into three nested loops like this,
   // where the dataflow directive would be applied to L3:
   // 
   // L1: for (int i = 0; i < OUTPUT_HEIGHT; i++) {
   //    L2: for (int j = 0; j < OUTPUT_WIDTH; j++) {
   //       L3: for (int k = 0; k < OUTPUT_CHANS / OCHAN_SCALE_FACTOR; k++) {
   //          (loop body)
   //       }
   //    }
   // }
   //
   // While this does technically work with the dataflow optimization, the synthesizer
   // is unable to properly flatten the three loops such that all calls to the dataflow
   // pipeline occur in one single contiguous stream. Instead, only (L3 trip count) calls 
   // are made in a row, and then L2 cannot begin its next iteration until the dataflow
   // pipeline is completely empty. Telling the synthesizer to explicitly flatten the loops
   // only makes the problem worse and causes the dataflow optimization to fail entirely.
   //
   // So instead, we must explicitly flatten the loops in the C code itself. The "get_next_ijk"
   // function will keep track of what the values of i,j,k would be if the loops were written 
   // as shown above.
   //
   // TODO: Figure out if this is fixed in Vitis.
   TOP_LOOP: for (int f = 0; f < TOP_LOOP_ITERATIONS; f++) {
      #pragma HLS stable variable=filter_data
      #pragma HLS stable variable=adjustments
      data_t ifmap_vec[FILTER_SIZE][FILTER_SIZE][INPUT_CHANS];
      data_t weight_vecs[OCHAN_SCALE_FACTOR][FILTER_SIZE][FILTER_SIZE][INPUT_CHANS];
      data_t products[OCHAN_SCALE_FACTOR][VECTOR_SIZE];
      data_t sums[OCHAN_SCALE_FACTOR];
      data_t outputs[OCHAN_SCALE_FACTOR];
      #pragma HLS array_partition variable=sums complete
      #pragma HLS array_partition variable=outputs complete
      uint16_t indices[3];
      #pragma HLS array_partition variable=indices complete
      tdf12_get_next_ijk(indices);
      uint16_t i_int = indices[0];
      uint16_t j_int = indices[1];
      uint16_t k_int = indices[2];
      // FOR EACH OUTPUT ELEMENT:
      //  - Read the convolution window of inputs
      //  - Read the filters
      //  - Perform element-wise multiplication of the inputs and weights
      //  - Accumulate the results
      //  - Adjust the sums (batch normalization, bias, activation)
      //  - Write the outputs.
      //
      //  Note that we can process multiple filters / output channels at the same time.
      tdf12_readInputs(in_data, i_int, j_int, ifmap_vec);
      tdf12_readFilters(filter_data, k_int, weight_vecs);
      tdf12_dot_product(ifmap_vec, weight_vecs, products);
      data_t accum1_out_0[32];
      data_t accum1_out_1[32];
      data_t accum1_out_2[32];
      data_t accum1_out_3[32];
      data_t accum1_out_4[32];
      #pragma HLS array_partition variable=accum1_out_0 cyclic factor=8
      #pragma HLS array_partition variable=accum1_out_1 cyclic factor=8
      #pragma HLS array_partition variable=accum1_out_2 cyclic factor=8
      #pragma HLS array_partition variable=accum1_out_3 cyclic factor=8
      #pragma HLS array_partition variable=accum1_out_4 cyclic factor=8
      tdf12_accum_1(products[0], accum1_out_0);
      tdf12_accum_1(products[1], accum1_out_1);
      tdf12_accum_1(products[2], accum1_out_2);
      tdf12_accum_1(products[3], accum1_out_3);
      tdf12_accum_1(products[4], accum1_out_4);
      data_t accum2_out_0[4];
      data_t accum2_out_1[4];
      data_t accum2_out_2[4];
      data_t accum2_out_3[4];
      data_t accum2_out_4[4];
      #pragma HLS array_partition variable=accum2_out_0 complete
      #pragma HLS array_partition variable=accum2_out_1 complete
      #pragma HLS array_partition variable=accum2_out_2 complete
      #pragma HLS array_partition variable=accum2_out_3 complete
      #pragma HLS array_partition variable=accum2_out_4 complete
      tdf12_accum_2(accum1_out_0, accum2_out_0);
      tdf12_accum_2(accum1_out_1, accum2_out_1);
      tdf12_accum_2(accum1_out_2, accum2_out_2);
      tdf12_accum_2(accum1_out_3, accum2_out_3);
      tdf12_accum_2(accum1_out_4, accum2_out_4);
      sums[0] = tdf12_accum_3(accum2_out_0);
      sums[1] = tdf12_accum_3(accum2_out_1);
      sums[2] = tdf12_accum_3(accum2_out_2);
      sums[3] = tdf12_accum_3(accum2_out_3);
      sums[4] = tdf12_accum_3(accum2_out_4);

      tdf12_adjust(sums, outputs, adjustments, k_int);
      tdf12_writeOutputs_unaligned(i_int, j_int, k_int, outputs, out_data);
   }
}

// Top-level wrapper function for tdf12
// The output data is a port so that when we calculate cost, we don't double-count
// the UltraRAMs (since output of one layer is input to the next one).
void tdf12_top(data_t dummy_val, data_t out_data[OUTPUT_HEIGHT][OUTPUT_WIDTH][OUTPUT_CHANS]) {
   data_t in_data[INPUT_HEIGHT][INPUT_WIDTH][INPUT_CHANS_PADDED];
   data_t filter_data[OUTPUT_CHANS][FILTER_SIZE][FILTER_SIZE][INPUT_CHANS];
   data_t adjustments[OUTPUT_CHANS][4];
   // Write one element to filters and adjustments to prevent tools from optimizing
   // them out. This is just to make sure the resource estimates are accurate.
   filter_data[0][0][0][0] = dummy_val;
   adjustments[0][0] = dummy_val;
   tdf12(in_data, out_data, filter_data, adjustments);
}

