#include "global_defines.h"
#include "tdf3_impl_defines.h"
#include <stdbool.h>
#include <assert.h>

#include "tdf3_conv_stages.h"

//////////////////////////////////////////////////////////////
//  ACCUMULATION FUNCTIONS
//////////////////////////////////////////////////////////////

// Accumulation stage 1
// This is a pipelined tree accumulation stage
// It reduces 32 inputs to 8 outputs.
// The estimated latency is 15 cycles.
void tdf3_accum_1(
   data_t accum_in[32], 
   data_t accum_out[8]
) {
   uint16_t out_idx = 0;
   IL_LOOP: for (uint16_t i1 = 0; i1 < 8; i1++) {
      uint16_t i = i1 * 4;
      #pragma HLS pipeline
      data_t vals[4];
      #pragma HLS array_partition variable=vals complete
      // This loop will be automatically unrolled and ideally all 
      // iterations of it must be scheduled in the same cycle.
      WRPC_LOOP: for (uint16_t w = 0; w < 4; w++) {
         // Need this bounds check because input length is not necessarily
         // a multiple of words read per cycle.
         vals[w] = (i+w < 32) ? accum_in[i+w] : (data_t)0;
      }
      data_t sum0 = vals[3] + vals[2];
      data_t sum1 = vals[1] + vals[0];
      data_t sum2 = sum0 + sum1;
      accum_out[out_idx+0] = sum2;
      out_idx += 1;

   }
}



// Accumulation stage 2
// This is a pipelined tree accumulation stage
// It reduces 8 inputs to 4 outputs.
// The estimated latency is 8 cycles.
void tdf3_accum_2(
   data_t accum_in[8], 
   data_t accum_out[4]
) {
   uint16_t out_idx = 0;
   IL_LOOP: for (uint16_t i1 = 0; i1 < 4; i1++) {
      uint16_t i = i1 * 2;
      #pragma HLS pipeline
      data_t vals[2];
      #pragma HLS array_partition variable=vals complete
      // This loop will be automatically unrolled and ideally all 
      // iterations of it must be scheduled in the same cycle.
      WRPC_LOOP: for (uint16_t w = 0; w < 2; w++) {
         // Need this bounds check because input length is not necessarily
         // a multiple of words read per cycle.
         vals[w] = (i+w < 8) ? accum_in[i+w] : (data_t)0;
      }
      data_t sum0 = vals[1] + vals[0];
      accum_out[out_idx+0] = sum0;
      out_idx += 1;

   }
}



// Accumulation stage 3
// This is a "simple" accumulation stage.
// It reduces 4 inputs to 1 output.
// The estimated latency is 13 cycles.
data_t tdf3_accum_3(data_t accum_in[4]) {
   data_t sum = 0.0;
   for (int i = 0; i < 4; i++) sum += accum_in[i];
   return sum;
}



// Function that keeps track of indices i,j,k for the top loop
// i and j are the output row and column coordinates, respectively
// k represents the output channel, but not directly. It actually 
// represents the group of output channels, since we can parallelize
// mutliple output channels for the same output XY coordinate. 
// For example, if OCHAN_SCALE_FACTOR = 4 (meaning we process 4 output channels
// at the same time), then k = 1 represents output channels 4,5,6,7.
void tdf3_get_next_ijk (uint16_t indices[3]) {
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


void tdf3 (
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
      tdf3_get_next_ijk(indices);
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
      tdf3_readInputs(in_data, i_int, j_int, ifmap_vec);
      tdf3_readFilters(filter_data, k_int, weight_vecs);
      tdf3_dot_product(ifmap_vec, weight_vecs, products);
      data_t accum1_out_0[8];
      data_t accum1_out_1[8];
      data_t accum1_out_2[8];
      data_t accum1_out_3[8];
      data_t accum1_out_4[8];
      data_t accum1_out_5[8];
      data_t accum1_out_6[8];
      data_t accum1_out_7[8];
      tdf3_accum_1(products[0], accum1_out_0);
      tdf3_accum_1(products[1], accum1_out_1);
      tdf3_accum_1(products[2], accum1_out_2);
      tdf3_accum_1(products[3], accum1_out_3);
      tdf3_accum_1(products[4], accum1_out_4);
      tdf3_accum_1(products[5], accum1_out_5);
      tdf3_accum_1(products[6], accum1_out_6);
      tdf3_accum_1(products[7], accum1_out_7);
      data_t accum2_out_0[4];
      data_t accum2_out_1[4];
      data_t accum2_out_2[4];
      data_t accum2_out_3[4];
      data_t accum2_out_4[4];
      data_t accum2_out_5[4];
      data_t accum2_out_6[4];
      data_t accum2_out_7[4];
      #pragma HLS array_partition variable=accum2_out_0 complete
      #pragma HLS array_partition variable=accum2_out_1 complete
      #pragma HLS array_partition variable=accum2_out_2 complete
      #pragma HLS array_partition variable=accum2_out_3 complete
      #pragma HLS array_partition variable=accum2_out_4 complete
      #pragma HLS array_partition variable=accum2_out_5 complete
      #pragma HLS array_partition variable=accum2_out_6 complete
      #pragma HLS array_partition variable=accum2_out_7 complete
      tdf3_accum_2(accum1_out_0, accum2_out_0);
      tdf3_accum_2(accum1_out_1, accum2_out_1);
      tdf3_accum_2(accum1_out_2, accum2_out_2);
      tdf3_accum_2(accum1_out_3, accum2_out_3);
      tdf3_accum_2(accum1_out_4, accum2_out_4);
      tdf3_accum_2(accum1_out_5, accum2_out_5);
      tdf3_accum_2(accum1_out_6, accum2_out_6);
      tdf3_accum_2(accum1_out_7, accum2_out_7);
      sums[0] = tdf3_accum_3(accum2_out_0);
      sums[1] = tdf3_accum_3(accum2_out_1);
      sums[2] = tdf3_accum_3(accum2_out_2);
      sums[3] = tdf3_accum_3(accum2_out_3);
      sums[4] = tdf3_accum_3(accum2_out_4);
      sums[5] = tdf3_accum_3(accum2_out_5);
      sums[6] = tdf3_accum_3(accum2_out_6);
      sums[7] = tdf3_accum_3(accum2_out_7);

      tdf3_adjust(sums, outputs, adjustments, k_int);
      tdf3_writeOutputs_aligned(i_int, j_int, k_int, outputs, out_data);
   }
}

// Top-level wrapper function for tdf3
// The output data is a port so that when we calculate cost, we don't double-count
// the UltraRAMs (since output of one layer is input to the next one).
void tdf3_top(data_t dummy_val, data_t out_data[OUTPUT_HEIGHT][OUTPUT_WIDTH][OUTPUT_CHANS]) {
   data_t in_data[INPUT_HEIGHT][INPUT_WIDTH][INPUT_CHANS_PADDED];
   data_t filter_data[OUTPUT_CHANS][FILTER_SIZE][FILTER_SIZE][INPUT_CHANS];
   data_t adjustments[OUTPUT_CHANS][4];
   // Write one element to filters and adjustments to prevent tools from optimizing
   // them out. This is just to make sure the resource estimates are accurate.
   filter_data[0][0][0][0] = dummy_val;
   adjustments[0][0] = dummy_val;
   tdf3(in_data, out_data, filter_data, adjustments);
}

