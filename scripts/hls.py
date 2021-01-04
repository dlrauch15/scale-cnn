# hls.py
# Code to interface with the Vitis HLS tool
import os
import hls_reports
import ast

# Given a layer name and a path to an implementation of that layer,
# calls vitis_hls to synthesize it.
def synthesize_layer(layer_name, impl_path):
   print("Synthesizing layer implementation at {}".format(impl_path))
   cwd = os.getcwd()
   os.chdir(impl_path)
   exitcode = os.WEXITSTATUS(os.system('vitis_hls -f {}.tcl > /dev/null'.format(layer_name)))
   os.chdir(cwd)
   if exitcode != 0:
      raise Exception('Vitis HLS failed with exit code {} at {}'.format(exitcode, impl_path))
   print("Done.")


# Also calculate the memory read bandwidth utilization of the layer implementation
# Memory read bandwidth utilization is calculated as follows:
# TD = Total amount of data that needs to be read
# CR = Total amount of data that can be read in one clock cycle
# TC = Total cycles the function takes to complete
# Memory read bandwidth utilization = (TD / CR) / TC
# Total data can be calculated as:
# TD = # input data to read per output element * # output elements
#    = (filter_size^2 * input_chans) * (output_height * output_width * output_chans) 
#def CalcMemReadBandwidthUtil(layer_spec, implementation, total_cycles):
#   total_words_to_read = (layer_spec['filter_size'] ** 2) * \
#                          layer_spec['input_chans']   * \
#                          layer_spec['output_height'] * \
#                          layer_spec['output_width']  * \
#                          layer_spec['output_chans']
#   # Right now, the memory read bandwidth is just the scale factor.
#   mem_read_bandwidth = implementation['read_scale_factor']
#   return (total_words_to_read / mem_read_bandwidth) / total_cycles


# Calculate the "true" latency of the function
# This is necessary because right now, the function only iterates on a very small
# subset of the data to reduce synthesis times. Since the top loop is a dataflow
# pipeline, we can easily calculate the true latency by adding the number of "skipped"
# iterations and multiplying it by the dataflow pipeline's II.
def CalcTrueLatency(layer_spec, impl_spec, report_latency, dataflow_ii):
   true_iters  = layer_spec['output_height'] * layer_spec['output_width'] * \
                 layer_spec['output_chans'] / impl_spec['ochan_scale_factor']
   synth_iters = 50 * layer_spec['output_chans'] / impl_spec['ochan_scale_factor']
   return int(report_latency + (true_iters - synth_iters) * dataflow_ii)


# Parse and analyze the reports after synthesis of a layer completes.
# The top-level report will contain cost info and overall latency info
# And it will tell us all the functions inside the dataflow-optimized loop.
# We can then read the reports of all the sub-functions to get their latencies
# and initiation intervals.
#
# This function returns a dictionary summarizing the results. The contents of 
# the dictionary are as follows:
#
#  - latency: The total number of cycles this iteration takes.
#  - cost_info: Another dictionary summarizing the resource cost
#  - subfunctions: A list that summarizes the subfunctions within the top loop.
#        Every element is a dictionary with this format:
#        - name: The name of the subfunction
#        - latency: The latency of the subfunction
#
# Additionally, a summary file with this data will be generated in the 
# implementation's root directory.
def analyze_reports(layer_spec, impl):

   layer_name = layer_spec['layer_name']
   impl_dir = impl['dir']

   # First, copy the report directory to the implementation root directory,
   # and delete everything else generated by the HLS tool. This is to save
   # disk space since the HLS tool can generate several MB worth of data
   # per implementation. All we really want is the reports.
   hls_proj_dir = os.path.join(impl_dir, '{}_prj'.format(layer_name))
   old_report_dir = os.path.join(hls_proj_dir, 'solution1/syn/report')
   os.system('cp -R {} {}'.format(old_report_dir, impl_dir))
   report_dir = os.path.join(impl_dir, 'report')
   # Delete everything generated by the HLS tool
   os.system('rm -rf {}'.format(hls_proj_dir))

   # Now we want to analyze the reports.
   # First we must read the top-level report
   # Then we need to read the reports for all of the sub-functions.
   # The top-level report file is named "(layer_name)_top_csynth_report.xml"

   # Get the latencies of each individual dataflow pipline stage
   dataflow_rpt_filepath = os.path.join(report_dir, 'dataflow_in_loop_TOP_LOOP_csynth.rpt')
   stage_latencies, dataflow_ii = hls_reports.GetDataflowStageLatencies(dataflow_rpt_filepath)

   top_level_rpt_filepath = os.path.join(report_dir, '{}_top_csynth.xml'.format(layer_name))
   top_level_xml = hls_reports.read_report_xml(top_level_rpt_filepath)
   top_latency_raw   = hls_reports.GetWorstCaseLatency(top_level_xml)
   top_latency_true  = CalcTrueLatency(layer_spec, impl, top_latency_raw, dataflow_ii)
   top_cost_info = hls_reports.GetCostInfo(top_level_xml)

   report_info = {}
   report_info['latency']      = top_latency_raw
   report_info['true_latency'] = top_latency_true
   report_info['cost_info']    = top_cost_info
   report_info['subfunctions'] = stage_latencies
   #report_info['mbru']         = CalcMemReadBandwidthUtil(layer_spec, impl, top_latency)
   return report_info


REPORT_HEADER = '''
===========================================================
== Synthesis results for {} layer implementations
==========================================================='''

def generate_layer_summary(layer_spec, summary_filepath, impl_results):
   global REPORT_HEADER

   # First generate a CSV file that just contains (implementation directory, latency, cost)
   # This will be used when we are analyzing multiple layers for a network.
   csv_filepath = summary_filepath + ".csv"
   with open(csv_filepath, 'w') as csv_file:
      csv_file.write('ImplementationDir,Latency,Cost\n')
      for impl, report_info in impl_results:
         latency = report_info['true_latency']
         cost    = report_info['cost_info']['total']
         csv_file.write(",".join([impl['dir'], str(latency), str(cost)]))
         csv_file.write('\n')

   # Now generate the human-readable file summarizing the different implementations
   # For each implementation, show what the sub-functions are, and the latencies of each.
   # Point out which stage is the longest.
   rpt_filepath = summary_filepath + ".txt"
   with open(rpt_filepath, 'w') as rpt:
      rpt.write(REPORT_HEADER.format(layer_spec['layer_name']))
      for impl, report_info in impl_results:
         rpt.write('\n\n')
         # Implementation name and directory
         rpt.write("Implementation: {}\n".format(impl['name']))
         rpt.write("Directory: {}\n".format(impl['dir']))
         # Report Info
         # Total latency
         rpt.write("\nTotal latency (raw)  : {} cycles\n".format('{:,}'.format(report_info['latency'])))
         rpt.write("Total latency (true) : {} cycles\n\n".format('{:,}'.format(report_info['true_latency'])))
         # Cost info
         cost_info = report_info['cost_info']
         rpt.write("Cost info:\n")
         # Report each individual cost and the total
         for cost_factor in cost_info:
            if cost_factor != 'total':
               rpt.write("{}: {:.2f}%\n".format(cost_factor, cost_info[cost_factor] * 100))
         rpt.write("Total cost: {:.3f}\n\n".format(cost_info['total']))
         # Subfunction latencies
         rpt.write("Subfunction latencies:\n")
         subfunctions = report_info['subfunctions']
         for func in subfunctions:
            rpt.write("{}: {} cycles\n".format(func['name'], func['latency']))
         # And finally, report memory read bandwidth utilization
         # Disabling this as the metric doesn't really make sense any more.
         #rpt.write('\nMemory Read Bandwidth Utilization: {:.1f}%\n'.format(report_info['mbru'] * 100))

   # Print the entire report to stdout and then print messages about the generated files.
   os.system('cat ' + rpt_filepath)
   print("\n\nGenerated above report at {}".format(rpt_filepath))
   print("Generated CSV summary at {}\n".format(csv_filepath))


# Top-level function called from scale-cnn.py to explore the different
# implementations for a layer.
#
# For each implementation, it synthesizes the layer and parses the report.
# Then it will analyze the results of all the implementations and report a summary.
def explore_layer_implementations(layer_spec, impl_list_path):

   # Read the file with the implementation paths to explore.
   implementations = []
   with open(impl_list_path, 'r') as f:
      lines = f.readlines()
      for line in lines:
         implementations.append(ast.literal_eval(line.rstrip('\n')))

   layer_name = layer_spec['layer_name']
   print("Exploring {} layer implementations for {}.".format(len(implementations), layer_name))

   implementation_results = []

   # For each implementation...
   for impl in implementations:
      if not os.path.isdir(impl['dir']):
         raise Exception('Invalid implementation path at {}'.format(impl_dir))
      # Synthesize the layer
      synthesize_layer(layer_name, impl['dir'])
      # Parse the reports to get performance and cost info
      report_info = analyze_reports(layer_spec, impl)
      implementation_results.append((impl, report_info))
      
   # Summarize the results
   summary_filename = '{}_implementations_summary'.format(layer_name)
   summary_filepath = os.path.join(os.path.dirname(impl_list_path), summary_filename)
   generate_layer_summary(layer_spec, summary_filepath, implementation_results)

