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


#ifndef GRAPHLAB_QUEUED_FIFO_SCHEDULER_HPP
#define GRAPHLAB_QUEUED_FIFO_SCHEDULER_HPP

#include <algorithm>
#include <queue>


#include <graphlab/graph/graph_basic_types.hpp>
#include <graphlab/parallel/pthread_tools.hpp>
#include <graphlab/parallel/atomic.hpp>


#include <graphlab/scheduler/ischeduler.hpp>
#include <graphlab/scheduler/terminator/iterminator.hpp>
#include <graphlab/scheduler/vertex_map.hpp>

#include <graphlab/scheduler/terminator/critical_termination.hpp>
#include <graphlab/options/options_map.hpp>


#include <graphlab/macros_def.hpp>
namespace graphlab {

  /**
   * \ingroup group_schedulers 
   *
   * This class defines a multiple queue approximate fifo scheduler.
   * Each processor has its own in_queue which it puts new tasks in
   * and out_queue which it pulls tasks from.  Once a processors
   * in_queue gets too large, the entire queue is placed at the end of
   * the shared master queue.  Once a processors out queue is empty it
   * grabs the next out_queue from the master.
   */
  template<typename Message>
  class queued_fifo_scheduler : public ischeduler<Message> {
  
  public:

    typedef Message message_type;

    typedef std::deque<vertex_id_type> queue_type;

  private:
    vertex_map<message_type> messages;
    std::deque<queue_type> master_queue;
    mutex master_lock;
    size_t sub_queue_size;
    std::vector<queue_type> in_queues;
    std::vector<mutex> in_queue_locks;
    std::vector<queue_type> out_queues;
    // Terminator
    critical_termination term;

  public:

    queued_fifo_scheduler(size_t num_vertices,
                          size_t ncpus,
                          const options_map& opts) :
      messages(num_vertices), 
      sub_queue_size(100), 
      in_queues(ncpus), in_queue_locks(ncpus), 
      out_queues(ncpus), term(ncpus) { 
      opts.get_option("queuesize", sub_queue_size);
    }

    void start() { 
      master_lock.lock();
      for (size_t i = 0;i < in_queues.size(); ++i) {
        master_queue.push_back(in_queues[i]);
        in_queues[i].clear();
      }
      master_lock.unlock();
      term.reset(); 
    }

    void schedule(const vertex_id_type vid, 
                  const message_type& msg) {      
      if (messages.add(vid, msg)) {
        const size_t cpuid = random::rand() % in_queues.size();
        in_queue_locks[cpuid].lock();
        queue_type& queue = in_queues[cpuid];
        queue.push_back(vid);
        if(queue.size() > sub_queue_size) {
          master_lock.lock();
          queue_type emptyq;
          master_queue.push_back(emptyq);
          master_queue.back().swap(queue);
          master_lock.unlock();
        }
        in_queue_locks[cpuid].unlock();
        term.new_job(cpuid);
      } 
    } // end of schedule

    void schedule_from_execution_thread(const size_t cpuid,
                                        const vertex_id_type vid, 
                                        const message_type& msg) {      
      if (messages.add(vid, msg)) {
        ASSERT_LT(cpuid, in_queues.size());
        in_queue_locks[cpuid].lock();
        queue_type& queue = in_queues[cpuid];
        queue.push_back(vid);
        if(queue.size() > sub_queue_size) {
          master_lock.lock();
          queue_type emptyq;
          master_queue.push_back(emptyq);
          master_queue.back().swap(queue);
          master_lock.unlock();
        }
        in_queue_locks[cpuid].unlock();
        term.new_job(cpuid);
      } 
    } // end of schedule

    void schedule_all(const message_type& msg,
                      const std::string& order) {
      if(order == "shuffle") {
        std::vector<vertex_id_type> permutation = 
          random::permutation<vertex_id_type>(messages.size());       
        foreach(vertex_id_type vid, permutation)  schedule(vid, msg);
      } else {
        for (vertex_id_type vid = 0; vid < messages.size(); ++vid)
          schedule(vid, msg);      
      }
    } // end of schedule_all

    void completed(const size_t cpuid,
                   const vertex_id_type vid,
                   const message_type& msg) {
      term.completed_job();
    }


    sched_status::status_enum 
    get_specific(vertex_id_type vid,
                 message_type& ret_msg) {
      bool get_success = messages.test_and_get(vid, ret_msg); 
      if (get_success) return sched_status::NEW_TASK;
      else return sched_status::EMPTY;
    }

    void place(vertex_id_type vid,
                 const message_type& msg) {
      messages.add(vid, msg);
    }


    void schedule_from_execution_thread(size_t cpuid, vertex_id_type vid) {
      if (messages.has_task(vid)) {
        ASSERT_LT(cpuid, in_queues.size());
        in_queue_locks[cpuid].lock();
        queue_type& queue = in_queues[cpuid];
        queue.push_back(vid);
        if(queue.size() > sub_queue_size) {
          master_lock.lock();
          queue_type emptyq;
          master_queue.push_back(emptyq);
          master_queue.back().swap(queue);
          master_lock.unlock();
        }
        in_queue_locks[cpuid].unlock();
        term.new_job(cpuid);
      }
    }

    void schedule(vertex_id_type vid) {
      if (messages.has_task(vid)) {
        const size_t cpuid = random::rand() % in_queues.size();
        in_queue_locks[cpuid].lock();
        queue_type& queue = in_queues[cpuid];
        queue.push_back(vid);
        if(queue.size() > sub_queue_size) {
          master_lock.lock();
          queue_type emptyq;
          master_queue.push_back(emptyq);
          master_queue.back().swap(queue);
          master_lock.unlock();
        }
        in_queue_locks[cpuid].unlock();
        term.new_job(cpuid);
      }
    }
    
    /** Get the next element in the queue */
    sched_status::status_enum get_next(const size_t cpuid,
                                       vertex_id_type& ret_vid,
                                       message_type& ret_msg) {
      // if the local queue is empty try to get a queue from the master
      while(1) {
        if(out_queues[cpuid].empty()) {
          master_lock.lock();
          if(!master_queue.empty()) {
            out_queues[cpuid].swap(master_queue.front());
            master_queue.pop_front();
          }
          master_lock.unlock();
        }
        // if the local queue is still empty see if there is any local
        // work left
        in_queue_locks[cpuid].lock();
        if(out_queues[cpuid].empty() && !in_queues[cpuid].empty()) {
          out_queues[cpuid].swap(in_queues[cpuid]);
        }
        in_queue_locks[cpuid].unlock();
        // end of get next
        queue_type& queue = out_queues[cpuid];
        if(!queue.empty()) {
          ret_vid = queue.front();
          queue.pop_front();
          if(messages.test_and_get(ret_vid, ret_msg)) {
            return sched_status::NEW_TASK;
          }
        } else {
          return sched_status::EMPTY;
        }
      }
    } // end of get_next_task

    iterminator& terminator() { return term; }

    size_t num_joins() const {
      return messages.num_joins();
    }
    /**
     * Print a help string describing the options that this scheduler
     * accepts.
     */
    static void print_options_help(std::ostream& out) { 
      out << "\t queuesize=100: the size at which a subqueue is "
          << "placed in the master queue" << std::endl;
    }


  }; 


} // end of namespace graphlab
#include <graphlab/macros_undef.hpp>

#endif

