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


#ifndef GRAPHLAB_FS_UTIL
#define GRAPHLAB_FS_UTIL

#include <string>
#include <vector>


namespace graphlab {

  namespace fs_util {

    /**
     * List all the files with the given suffix at the pathname
     * location
     */
    void list_files_with_suffix(const std::string& pathname,
                                const std::string& suffix,
                                std::vector<std::string>& files);


    /**
     * List all the files with the given prefix at the pathname
     * location
     */
    void list_files_with_prefix(const std::string& pathname,
                                const std::string& prefix,
                                std::vector<std::string>& files,
                                bool includedir = false);

    /**
     * Concatenation two string to form a legal filesystem path by 
     * checking the trailing "/" of the first string.
     */
    std::string concat_path(const std::string& base,
                                     const std::string& suffix);


    /// \ingroup util_internal
    std::string change_suffix(const std::string& fname,
                                     const std::string& new_suffix);


  }; // end of fs_utils


}; // end of graphlab
#endif

