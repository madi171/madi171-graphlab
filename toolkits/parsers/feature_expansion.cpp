/**  
 * Copyright (c) 2009 Carnegie Mellon University. 
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://www.graphlab.ml.cmu.edu
 *
 */


#include <cstdio>
#include <map>
#include <iostream>
#include "graphlab.hpp"
#include "../shared/io.hpp"
#include "../shared/types.hpp"
using namespace graphlab;
using namespace std;


bool debug = false;
bool quick = true;
bool gzip = false;
string user_data_file;
string item_data_file;
string training_data, validation_data;
string keywords_data_file;
int nodes = 2421057;
int split_training_time = 1320595199;
int MAX_FEATURE = 410;
int pos_offset = 0;
const int MATRIX_MARKET = 1;
const int VW = 2;
int output_format = 1;

struct vertex_data {
  vertex_data(){ 
  }
  void add_self_edge(double value) { }

  void set_val(double value, int field_type) { 
  }  
  double get_output(int field_type){ return NAN; }
}; 

struct edge_data2 {
  int value;
  int time;
  edge_data2(int value) : value(value) { time = 0; }
  edge_data2(int value, int time) : value(value), time(time) { }
  void set_field(int pos, int val){
     time = val;
  }
  int get_field(int pos){ if (pos == 0) return value; else if (pos == 1) return time; else assert(false); }
};

struct edge_data{
  double weight;
  edge_data(double weight): weight(weight) { }
  void set_field(int pos, double val){}
  double get_field(int pos) { return weight; }
};

typedef graphlab::graph<vertex_data, edge_data> graph_type;
typedef graphlab::graph<vertex_data, edge_data2> graph_type2;
typedef graph_type::edge_list_type edge_list;
typedef graph_type2::edge_list_type edge_list2;

int main(int argc,  char *argv[]) {
  int col = 0;  
  global_logger().set_log_level(LOG_INFO);
  global_logger().set_log_to_console(true);

  graphlab::command_line_options clopts("GraphLab Parsers  Library");

  clopts.attach_option("user_data", &user_data_file, user_data_file,
                       "user feature input file");
  clopts.add_positional("user_data");
  clopts.attach_option("item_data", &item_data_file, item_data_file, "item feature data file");
  clopts.add_positional("item_data");
  clopts.attach_option("debug", &debug, debug, "Display debug output.");
  clopts.attach_option("gzip", &gzip, gzip, "Gzipped input file?");
  clopts.attach_option("output_format", &output_format, output_format, "output format 1=Matrix market, 2=VW");
  clopts.attach_option("max_feature", &MAX_FEATURE, MAX_FEATURE, "max number of feature");
  clopts.attach_option("col", &col, col, "feature position to expand");

  // Parse the command line arguments
  if(!clopts.parse(argc, argv)) {
    std::cout << "Invalid arguments!" << std::endl;
    return EXIT_FAILURE;
  }

  logstream(LOG_WARNING)
    << "Eigen detected. (This is actually good news!)" << std::endl;
  logstream(LOG_INFO) 
    << "GraphLab parsers library code by Danny Bickson, CMU" 
    << std::endl 
    << "Send comments and bug reports to danny.bickson@gmail.com" 
    << std::endl 
    << "Currently implemented parsers are: Call data records, document tokens "<< std::endl;
  
  assert(col >=0 && col < MAX_FEATURE);


  timer mytimer; mytimer.start();
  graph_type2 training;
  graph_type user_data, item_data;
  bipartite_graph_descriptor item_data_info;
  load_matrixmarket_graph(item_data_file, item_data_info, item_data, MATRIX_MARKET_3, true);

  gzip_out_file fout(training_data + ".info", gzip);
  gzip_out_file ffout(training_data + ".data", gzip);
  gzip_in_file fin(training_data, gzip);
  MM_typecode out_typecode;
  mm_clear_typecode(&out_typecode);
  mm_set_real(&out_typecode); 
  mm_set_sparse(&out_typecode); 
  mm_set_matrix(&out_typecode);
  mm_write_cpp_banner(fout.get_sp(), out_typecode);

  char linebuf[10000]; 
  char saveptr[10000];
  int line = 0;
  while (!fin.get_sp().eof() && fin.get_sp().good()){ 

     fin.get_sp().getline(linebuf, 10000);
     if (fin.get_sp().eof())
        break;

      char *pch = strtok_r(linebuf," \r\n\t",(char**)&saveptr);
      if (!pch){
        logstream(LOG_FATAL) << "Error when parsing file: " << training_data << ":" << line <<std::endl;
       }
       ffout.get_sp()<<pch<<" ";
       
       for (int j=0; j< MAX_FEATURE-1; j++){
					pch = strtok_r(linebuf," \r\n\t",(char**)&saveptr);
          if (!pch){
            logstream(LOG_FATAL) << "Error when parsing file: " << training_data << ":" << line <<std::endl;
             
          ffout.get_sp()<<pch<< " ";

          if (col == j){
			       int pos = atoi(pch);
             if (pos < j || pos > MAX_FEATURE)
                logstream(LOG_FATAL)<<"Error on line: " << line << " position is not in range: " << pos << std::endl;
             if (item_data.out_edges(pos).size() == 0)
                logstream(LOG_FATAL)<<"Did not find features for: " << pos << std::endl;
					     for (uint k=0; k< item_data.out_edges(pos).size(); k++)
                 ffout.get_sp()<< item_data.out_edges(pos)[k].target()<< " ";
             
           }


          }
          if (j == MAX_FEATURE -2)
             ffout.get_sp()<<endl;
    } //for loop
    line++;
 }//while loop

  std::cout << "Finished in " << mytimer.current_time() << " total lines: " << line << std::endl;
  return EXIT_SUCCESS;
} //main



