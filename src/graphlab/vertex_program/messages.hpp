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

#ifndef GRAPHLAB_MESSAGES_HPP
#define GRAPHLAB_MESSAGES_HPP

#include <graphlab/serialization/serialization_includes.hpp>

namespace graphlab {

  namespace messages {

    /**
     * The priority of two messages is the sum
     */
    struct sum : public graphlab::IS_POD_TYPE {
      double prio;
      sum(const double prio = 0) : prio(prio) { }
      double priority() const { return prio; }
      sum& operator+=(const sum& other) {
        prio += other.prio;
        return *this;
      }
    }; // end of sum message

    /**
     * The priority of two messages is the max
     */
    struct max : public graphlab::IS_POD_TYPE {
      double prio;
      max(const double prio = 0) : prio(prio) { }
      double priority() const { return prio; }
      max& operator+=(const sum& other) {
        prio = std::max(prio, other.prio);
        return *this;
      }
    }; // end of max message


  }; // end of messages namespace


}; // end of graphlab namespace
#endif
