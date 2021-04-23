#include "global_defines.h"
#include "tdf2_impl_defines.h"
#include <stdbool.h>
#include <assert.h>

#include "tdf2_conv_stages.h"


// Pooling / writing function
// This function receives unpooled output elements and "pools" them by 
// calculating the running maximum. Once enough inputs have been gathered,
// it calls the writeOutput function with the maximum value.
void tdf2_poolOutputs (
   uint16_t i_out, uint16_t j_out, uint16_t k, 
   bool resetMaximum, bool storeOutput, 
   data_t outputs[OCHAN_SCALE_FACTOR], 
   data_t out_data[OUTPUT_HEIGHT][OUTPUT_WIDTH][OUTPUT_CHANS]
) {
   static data_t max_vals[OCHAN_SCALE_FACTOR];
   #pragma HLS array_partition variable=max_vals complete
   for (uint16_t o = 0; o < OCHAN_SCALE_FACTOR; o++) {
      #pragma HLS unroll
      bool greaterThanMaximum = (outputs[o] > max_vals[o]);
      max_vals[o] = (resetMaximum || greaterThanMaximum) ? outputs[o] : max_vals[o];
   }
   if (storeOutput) {
      tdf2_writeOutputs_aligned(i_out, j_out, k, max_vals, out_data);
   }
}


//////////////////////////////////////////////////////////////
//  ACCUMULATION FUNCTIONS
//////////////////////////////////////////////////////////////

// Accumulation stage 1
// This is a pipelined tree accumulation stage
// It reduces 144 inputs to 9 outputs.
// The estimated latency is 22 cycles.
void tdf2_accum_1(
   data_t accum_in[144], 
   data_t accum_out[9]
) {
   uint16_t out_idx = 0;
   IL_LOOP: for (uint16_t i1 = 0; i1 < 9; i1++) {
      uint16_t i = i1 * 16;
      #pragma HLS pipeline
      data_t vals[16];
      #pragma HLS array_partition variable=vals complete
      // This loop will be automatically unrolled and ideally all 
      // iterations of it must be scheduled in the same cycle.
      WRPC_LOOP: for (uint16_t w = 0; w < 16; w++) {
         // Need this bounds check because input length is not necessarily
         // a multiple of words read per cycle.
         vals[w] = (i+w < 144) ? accum_in[i+w] : (data_t)0;
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
      data_t sum14 = sum12 + sum13;
      accum_out[out_idx+0] = sum14;
      out_idx += 1;

   }
}



// Accumulation stage 2
// This is a pipelined tree accumulation stage
// It reduces 9 inputs to 5 outputs.
// The estimated latency is 9 cycles.
void tdf2_accum_2(
   data_t accum_in[9], 
   data_t accum_out[5]
) {
   uint16_t out_idx = 0;
   IL_LOOP: for (uint16_t i1 = 0; i1 < 5; i1++) {
      uint16_t i = i1 * 2;
      #pragma HLS pipeline
      data_t vals[2];
      #pragma HLS array_partition variable=vals complete
      // This loop will be automatically unrolled and ideally all 
      // iterations of it must be scheduled in the same cycle.
      WRPC_LOOP: for (uint16_t w = 0; w < 2; w++) {
         // Need this bounds check because input length is not necessarily
         // a multiple of words read per cycle.
         vals[w] = (i+w < 9) ? accum_in[i+w] : (data_t)0;
      }
      data_t sum0 = vals[1] + vals[0];
      accum_out[out_idx+0] = sum0;
      out_idx += 1;

   }
}



// Accumulation stage 3
// This is a "simple" accumulation stage.
// It reduces 5 inputs to 1 output.
// The estimated latency is 16 cycles.
data_t tdf2_accum_3(data_t accum_in[5]) {
   data_t sum = 0.0;
   for (int i = 0; i < 5; i++) sum += accum_in[i];
   return sum;
}



// Function that keeps track of indices i,j,k for the top loop
// i and j are the row and column coordinates of the unpooled outputs, respectively.
// k represents the output channel, but not directly. It actually 
// represents the group of output channels, since we can parallelize
// mutliple output channels for the same output XY coordinate. 
// For example, if OCHAN_SCALE_FACTOR = 4 (meaning we process 4 output channels
// at the same time), then k = 1 represents output channels 4,5,6,7.
//
// The order in which i,j,k change is very particular since we must account for the pooling
// that is done at the end of the dataflow pipeline. We cannot simply iterate over columns
// before moving to the next row. Instead, we must complete one "pooling window" before moving
// on to the next.
//
// For regular conv layers, we could iterate over the input coordinates as follows:
// (0,0), (0,1), (0,2), ... (0, INPUT_WIDTH-1)
//
// But if we have, for example, 2x2 pooling, we need this order:
// (0,0), (0,1), (1,0), (1,1), (0,2) ...
//
// This considerably simplifies the pooling function as otherwise it 
// would need a lot of intermediate storage to store unpooled values
// before it had all values in one pooling window.
void tdf2_get_next_ijk ( 
   uint16_t input_indices[3],
   uint16_t output_indices[2],
   bool *resetMaximum,
   bool *storeOutput 
) {
   static uint16_t i     = 0;
   static uint16_t j     = 0;
   static uint16_t k     = 0;
   static uint16_t i_out = 0;
   static uint16_t j_out = 0;
   static uint8_t  i_p   = 0;
   static uint8_t  j_p   = 0;
   assert(i_p <= POOLING_FACTOR);
   assert(j_p <= POOLING_FACTOR);
   *resetMaximum = (i_p == 0 && j_p == 0);
   *storeOutput  = (i_p == POOLING_FACTOR-1) && (j_p == POOLING_FACTOR-1);
   input_indices[0] = i + i_p;
   input_indices[1] = j + j_p;
   input_indices[2] = k;
   output_indices[0] = i_out;
   output_indices[1] = j_out;
   j_p++;
   if (j_p == POOLING_FACTOR) {
      j_p = 0;
      i_p++;
      if (i_p == POOLING_FACTOR) {
         i_p = 0;
         k++;
         if (k == OUTPUT_CHANS / OCHAN_SCALE_FACTOR) {
            k = 0;
            j += POOLING_FACTOR;
            j_out++;
            if (j_out == OUTPUT_WIDTH) {
               j = 0;
               j_out = 0;
               i += POOLING_FACTOR;
               i_out++;
               if (i_out == OUTPUT_HEIGHT) {
                  i = 0;
                  i_out = 0;
               }
            }
         }
      }
   }
}


void tdf2 (
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
      uint16_t input_indices[3];
      uint16_t output_indices[2];
      #pragma HLS array_partition variable=input_indices complete
      #pragma HLS array_partition variable=output_indices complete
      bool resetMaximum, storeOutput;
      tdf2_get_next_ijk(input_indices, output_indices, &resetMaximum, &storeOutput);
      uint16_t i_in  = input_indices[0];
      uint16_t j_in  = input_indices[1];
      uint16_t k     = input_indices[2];
      uint16_t i_out = output_indices[0];
      uint16_t j_out = output_indices[1];
      // FOR EACH OUTPUT ELEMENT:
      //  - Read the convolution window of inputs
      //  - Read the filters
      //  - Perform element-wise multiplication of the inputs and weights
      //  - Accumulate the results
      //  - Adjust the sums (batch normalization, bias, activation)
      //  - Write the outputs.
      //
      //  Note that we can process multiple filters / output channels at the same time.
      tdf2_readInputs(in_data, i_in, j_in, ifmap_vec);
      tdf2_readFilters(filter_data, k, weight_vecs);
      tdf2_dot_product(ifmap_vec, weight_vecs, products);
      data_t accum1_out_0[9];
      data_t accum1_out_1[9];
      data_t accum1_out_2[9];
      data_t accum1_out_3[9];
      data_t accum1_out_4[9];
      data_t accum1_out_5[9];
      data_t accum1_out_6[9];
      data_t accum1_out_7[9];
      data_t accum1_out_8[9];
      data_t accum1_out_9[9];
      data_t accum1_out_10[9];
      data_t accum1_out_11[9];
      data_t accum1_out_12[9];
      data_t accum1_out_13[9];
      data_t accum1_out_14[9];
      data_t accum1_out_15[9];
      tdf2_accum_1(products[0], accum1_out_0);
      tdf2_accum_1(products[1], accum1_out_1);
      tdf2_accum_1(products[2], accum1_out_2);
      tdf2_accum_1(products[3], accum1_out_3);
      tdf2_accum_1(products[4], accum1_out_4);
      tdf2_accum_1(products[5], accum1_out_5);
      tdf2_accum_1(products[6], accum1_out_6);
      tdf2_accum_1(products[7], accum1_out_7);
      tdf2_accum_1(products[8], accum1_out_8);
      tdf2_accum_1(products[9], accum1_out_9);
      tdf2_accum_1(products[10], accum1_out_10);
      tdf2_accum_1(products[11], accum1_out_11);
      tdf2_accum_1(products[12], accum1_out_12);
      tdf2_accum_1(products[13], accum1_out_13);
      tdf2_accum_1(products[14], accum1_out_14);
      tdf2_accum_1(products[15], accum1_out_15);
      data_t accum2_out_0[5];
      data_t accum2_out_1[5];
      data_t accum2_out_2[5];
      data_t accum2_out_3[5];
      data_t accum2_out_4[5];
      data_t accum2_out_5[5];
      data_t accum2_out_6[5];
      data_t accum2_out_7[5];
      data_t accum2_out_8[5];
      data_t accum2_out_9[5];
      data_t accum2_out_10[5];
      data_t accum2_out_11[5];
      data_t accum2_out_12[5];
      data_t accum2_out_13[5];
      data_t accum2_out_14[5];
      data_t accum2_out_15[5];
      #pragma HLS array_partition variable=accum2_out_0 complete
      #pragma HLS array_partition variable=accum2_out_1 complete
      #pragma HLS array_partition variable=accum2_out_2 complete
      #pragma HLS array_partition variable=accum2_out_3 complete
      #pragma HLS array_partition variable=accum2_out_4 complete
      #pragma HLS array_partition variable=accum2_out_5 complete
      #pragma HLS array_partition variable=accum2_out_6 complete
      #pragma HLS array_partition variable=accum2_out_7 complete
      #pragma HLS array_partition variable=accum2_out_8 complete
      #pragma HLS array_partition variable=accum2_out_9 complete
      #pragma HLS array_partition variable=accum2_out_10 complete
      #pragma HLS array_partition variable=accum2_out_11 complete
      #pragma HLS array_partition variable=accum2_out_12 complete
      #pragma HLS array_partition variable=accum2_out_13 complete
      #pragma HLS array_partition variable=accum2_out_14 complete
      #pragma HLS array_partition variable=accum2_out_15 complete
      tdf2_accum_2(accum1_out_0, accum2_out_0);
      tdf2_accum_2(accum1_out_1, accum2_out_1);
      tdf2_accum_2(accum1_out_2, accum2_out_2);
      tdf2_accum_2(accum1_out_3, accum2_out_3);
      tdf2_accum_2(accum1_out_4, accum2_out_4);
      tdf2_accum_2(accum1_out_5, accum2_out_5);
      tdf2_accum_2(accum1_out_6, accum2_out_6);
      tdf2_accum_2(accum1_out_7, accum2_out_7);
      tdf2_accum_2(accum1_out_8, accum2_out_8);
      tdf2_accum_2(accum1_out_9, accum2_out_9);
      tdf2_accum_2(accum1_out_10, accum2_out_10);
      tdf2_accum_2(accum1_out_11, accum2_out_11);
      tdf2_accum_2(accum1_out_12, accum2_out_12);
      tdf2_accum_2(accum1_out_13, accum2_out_13);
      tdf2_accum_2(accum1_out_14, accum2_out_14);
      tdf2_accum_2(accum1_out_15, accum2_out_15);
      sums[0] = tdf2_accum_3(accum2_out_0);
      sums[1] = tdf2_accum_3(accum2_out_1);
      sums[2] = tdf2_accum_3(accum2_out_2);
      sums[3] = tdf2_accum_3(accum2_out_3);
      sums[4] = tdf2_accum_3(accum2_out_4);
      sums[5] = tdf2_accum_3(accum2_out_5);
      sums[6] = tdf2_accum_3(accum2_out_6);
      sums[7] = tdf2_accum_3(accum2_out_7);
      sums[8] = tdf2_accum_3(accum2_out_8);
      sums[9] = tdf2_accum_3(accum2_out_9);
      sums[10] = tdf2_accum_3(accum2_out_10);
      sums[11] = tdf2_accum_3(accum2_out_11);
      sums[12] = tdf2_accum_3(accum2_out_12);
      sums[13] = tdf2_accum_3(accum2_out_13);
      sums[14] = tdf2_accum_3(accum2_out_14);
      sums[15] = tdf2_accum_3(accum2_out_15);

      tdf2_adjust(sums, outputs, adjustments, k);
      tdf2_poolOutputs(i_out, j_out, k, resetMaximum, storeOutput, outputs, out_data);
   }
}

// Top-level wrapper function for tdf2
// The output data is a port so that when we calculate cost, we don't double-count
// the UltraRAMs (since output of one layer is input to the next one).
void tdf2_top(data_t dummy_val, data_t out_data[OUTPUT_HEIGHT][OUTPUT_WIDTH][OUTPUT_CHANS]) {
   data_t in_data[INPUT_HEIGHT][INPUT_WIDTH][INPUT_CHANS_PADDED];
   data_t filter_data[OUTPUT_CHANS][FILTER_SIZE][FILTER_SIZE][INPUT_CHANS];
   data_t adjustments[OUTPUT_CHANS][4];
   // Write one element to filters and adjustments to prevent tools from optimizing
   // them out. This is just to make sure the resource estimates are accurate.
   filter_data[0][0][0][0] = dummy_val;
   adjustments[0][0] = dummy_val;
   tdf2(in_data, out_data, filter_data, adjustments);
}

