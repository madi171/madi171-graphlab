#ifndef GRAPHLAB_DISTRIBUTED_GRAPH_HPP
#define GRAPHLAB_DISTRIBUTED_GRAPH_HPP
#include <algorithm>
#include <graphlab/rpc/dc.hpp>
#include <graphlab/rpc/dc_dist_object.hpp>
#include <graphlab/rpc/caching_dht.hpp>
#include <graphlab/util/stl_util.hpp>
#include <graphlab/distributed2/graph/graph_local_store.hpp>
#include <graphlab/distributed2/graph/atom_index_file.hpp>
#include <graphlab/distributed2/graph/atom_file.hpp>
#include <graphlab/distributed2/graph/dgraph_edge_list.hpp>
#include <graphlab/logger/assertions.hpp>

#include <graphlab/macros_def.hpp>
namespace graphlab {


/**
 * \brief Distributed Graph Implementation.
 * 
 * A fully distributed implementation of the \ref graph object.
 * Vertices are partitioned across machines. Each vertex is owned by a
 * unique machine. Each edge is owned by its destination vertex.  Each
 * machine stores all vertex data for vertices within its partition,
 * as well as vertex data for vertices/edges on the boundary of the
 * partition.  Each vertex data instance is therefore replicated as
 * many times as the number of distinct machines owning neighbors of
 * the vertex in question.
 * 
 * Formally, where \f$ \Gamma(v)\f$ is the set of neighbors of \f$
 * v\f$ and \f$o(v)\f$ is the owner of vertex v, vertex v is
 * replicated \f$ \mbox{DISTINCT} \left( \left\{ o(u), u \in \left\{
 * v,\Gamma(v) \right\} \right\} \right) \f$ times.
 * 
 * Each edge is replicated a maximum of 2 times.
 * 
 * To standardize on terminology, we call the set of vertices and
 * edges owned by a machine, the machine's <b> partition </b>. We call
 * the set of vertices and edges adjacent to the partition (but not in
 * the partition), the <b> boundary </b>. Finally, we will call a
 * machine's local copy of the partition + boundary, the machine's <b>
 * fragment </b>.
 * 
 * 
 *
 * Vertex / Edge IDs: 
 * 
 * Every vertex/edge in the graph has a uniquely assigned global
 * vertex/edge ID.  The task of guaranteeing unique sequential
 * assignment is currently managed by machine 0.
 * 
 * \note If this is a performance issue in the future, the sequential
 * assignment guarantee could be dropped by having either a
 * post-graph-construction renumbering scheme, or by building the rest
 * of the components of GraphLab to not depend on sequential
 * numbering. The user should not expect the sequential numbering
 * property to be preserved in future versions.
 * 
 * Each machine has a local representation for its fragment of the
 * graph. Within the local fragment, each vertex/edge has a local
 * vertex/edge ID. This local ID is hidden and abstracted from the
 * user. Implementors however should keep in mind the following
 * requirements for the local representation:
 *
 * <ul>
 * <li> Local vertex / edge IDs are unique and sequentially assigned </li>
 * <li> Sorting all vertices/edges in the local fragment must
 *      produce the same sequence whether or not we sort by global IDs 
 *      or Local IDs. </li>
 * </ul>
 * 
 * Consistency: 
 * 
 * Consistency of graph data, is not managed and must be done manually
 * through the various synchronize() operations. All data reads will
 * be accessed through the local fragment if the local fragment
 * contains the data. Otherwise, it will be requested from the owner
 * of the data. All data writes will be sent to the owner of the
 * data. The writes may not however, update all fragments unless
 * explicitly requested.
 * 
 * 
 */
template<typename VertexData, typename EdgeData> 
class distributed_graph {
 
 public:
  distributed_graph(distributed_control &dc, std::string atomidxfile):
                              rmi(dc, this),
                              globalvid2owner(dc, 65536),
                              globaleid2owner(dc, 65536){
    edge_canonical_numbering = false;
    // read the atom index.
    atom_index_file atomindex = read_atom_index(atomidxfile);
    // store the graph size
    numglobalverts = atomindex.nverts;
    numglobaledges = atomindex.nedges;
    // machine 0 partitions it
    std::vector<std::vector<size_t> > partitions;
    if (dc.procid() == 0) {
      partitions = partition_atoms(atomindex, dc.numprocs());
    }
    dc.services().broadcast(partitions, dc.procid() == 0);
    construct_local_fragment(atomindex, partitions, rmi.procid());
  }

  /**
   * Returns the number of vertices in the graph.
   */
  size_t num_vertices() const{
      return numglobalverts;
  }

  /**
   * Returns the number of edges in the graph.
   */  
  size_t num_edges() const{
      return numglobaledges;
  }

  size_t num_in_neighbors(vertex_id_t vid) const {
    boost::unordered_map<vertex_id_t, vertex_id_t>::const_iterator iter = 
      global2localvid.find(vid);
    // if I have this vertex in my fragment
    if (iter != global2localvid.end()) {
      // and if I own it (it is interior)
      vertex_id_t localvid = iter->second;
      if (localvid2owner[localvid] == rmi.procid()) {
        return localstore.num_in_neighbors(localvid);
      }
    }
    std::pair<bool, procid_t> vidowner = globalvid2owner.get_cached(vid);
    assert(vidowner.first);
    // otherwise I need to ask the owner
    return rmi.remote_request(vidowner.second,
                              &distributed_graph<VertexData, EdgeData>::
                              num_in_neighbors,
                              vid);
  }


  size_t num_out_neighbors(vertex_id_t vid) const {
    boost::unordered_map<vertex_id_t, vertex_id_t>::const_iterator iter = 
      global2localvid.find(vid);
    // if I have this vertex in my fragment
    if (iter != global2localvid.end()) {
      // and if I own it (it is interior)
      vertex_id_t localvid = iter->second;
      if (localvid2owner[localvid] == rmi.procid()) {
        return localstore.num_out_neighbors(localvid);
      }
    }

    std::pair<bool, procid_t> vidowner = globalvid2owner.get_cached(vid);
    assert(vidowner.first);

    // otherwise I need to ask the owner
    return rmi.remote_request(vidowner.second,
                              &distributed_graph<VertexData, EdgeData>::
                              num_out_neighbors,
                              vid);
  }


  std::pair<bool, edge_id_t>
  find(vertex_id_t source, vertex_id_t target) const {
    std::pair<bool, edge_id_t> ret;
    // hmm. surprisingly tricky
    boost::unordered_map<vertex_id_t, vertex_id_t>::const_iterator itersource = 
      global2localvid.find(source);
    boost::unordered_map<vertex_id_t, vertex_id_t>::const_iterator itertarget = 
      global2localvid.find(target);
    // both are local, I can find it
    if (itersource != global2localvid.end() && 
        itertarget != global2localvid.end()) {
      ret = localstore.find(itersource->second, itertarget->second);
      // convert to global edge ids
      if (ret.first) ret.second = local2globaleid[ret.second];
      return ret;
    }
    // if the edge exists, the owner of either the source or target must have it
    // lets use the target
    std::pair<bool, procid_t> vidowner = globalvid2owner.get_cached(target);
    assert(vidowner.first);
    procid_t targetowner = vidowner.second;
    
    // if I am the owner, then this edge can't possibly exist
    if (targetowner == rmi.procid()) {
      ret.first = false; ret.second = 0;
      return ret;
    }
    else {
      return rmi.remote_request(targetowner,
                                &distributed_graph<VertexData, EdgeData>::find,
                                source,
                                target);
    }
  }

  // unsafe version of find
  edge_id_t edge_id(vertex_id_t source, vertex_id_t target) const {
    std::pair<bool, edge_id_t> res = find(source, target);
    // The edge must exist
    assert(res.first);
    return res.second;
  }

  edge_id_t rev_edge_id(edge_id_t eid) const {
    // do I have this edge in the fragment?
    boost::unordered_map<edge_id_t, edge_id_t>::const_iterator iter = 
      global2localeid.find(eid);

    // yup ! then I must have the reverse in my fragment too
    if (iter != global2localeid.end()) {
      // get the local store to reverse it, and convert back to global
      return local2globaleid[localstore.rev_edge_id(iter->second)];
    }
    else {
      ASSERT_MSG(edge_canonical_numbering == false,
                "Remote edge request impossible due to use of canonical edge numbering");
      std::pair<bool, procid_t> eidowner = globaleid2owner.get_cached(eid);
      assert(eidowner.first);

      // I don't have it. Lets ask the owner of the edge
      return rmi.remote_request(eidowner.second,
                                &distributed_graph<VertexData, EdgeData>::
                                rev_edge_id,
                                eid);
    }
  } // end of rev_edge_id


  /** \brief Returns the source vertex of an edge. */
  vertex_id_t source(edge_id_t eid) const {
    // do I have this edge in the fragment?
    boost::unordered_map<edge_id_t, edge_id_t>::const_iterator iter = 
      global2localeid.find(eid);
    if (iter != global2localeid.end()) {
        // yup!
      return local2globalvid[localstore.source(iter->second)];
    }
    else {
      ASSERT_MSG(edge_canonical_numbering == false,
                "Remote edge request impossible due to use of canonical edge numbering");

      std::pair<bool, procid_t> eidowner = globaleid2owner.get_cached(eid);
      assert(eidowner.first);

      // ask the owner
      return rmi.remote_request(eidowner.second,
                                &distributed_graph<VertexData, EdgeData>::source,
                                eid);
    }
  }

  /** \brief Returns the destination vertex of an edge. */
  vertex_id_t target(edge_id_t eid) const {
    // do I have this edge in the fragment?
    boost::unordered_map<edge_id_t, edge_id_t>::const_iterator iter = 
      global2localeid.find(eid);
    if (iter != global2localeid.end()) {
        // yup!
      return local2globalvid[localstore.target(iter->second)];
    }
    else {
      ASSERT_MSG(edge_canonical_numbering == false,
                "Remote edge request impossible due to use of canonical edge numbering");

      std::pair<bool, procid_t> eidowner = globaleid2owner.get_cached(eid);
      assert(eidowner.first);

      // ask the owner
      return rmi.remote_request(eidowner.second,
                                &distributed_graph<VertexData, EdgeData>::target,
                                eid);
    }
  }

    /** \brief Return the edge ids of the edges arriving at v */
  dgraph_edge_list in_edge_ids(vertex_id_t v) const {
    boost::unordered_map<vertex_id_t, vertex_id_t>::const_iterator iter = 
      global2localvid.find(v);
    // if I have the vertex in my fragment
    // and if it is interior
    if (iter != global2localvid.end()) {
      vertex_id_t localvid = iter->second;
      if (localvid2owner[localvid]  == rmi.procid()) {
        return dgraph_edge_list(localstore.in_edge_ids(localvid), 
                                local2globaleid);
      }
    }
    // ok I need to construct a vector
    return dgraph_edge_list(in_edge_id_as_vec(v));
  } // end of in edges

  std::vector<edge_id_t> in_edge_id_as_vec(vertex_id_t v) const {
    boost::unordered_map<vertex_id_t, vertex_id_t>::const_iterator iter = 
      global2localvid.find(v);
    // if I have the vertex in my fragment
    // and if it is interior
    std::vector<edge_id_t> ret;
    if (iter != global2localvid.end()) {
      vertex_id_t localvid = iter->second;
      if (localvid2owner[localvid]  == rmi.procid()) {
        foreach(edge_id_t localeid, localstore.in_edge_ids(localvid)) {
          ret.push_back(local2globaleid[localeid]);
        }
        return ret;
      }
    }
    std::pair<bool, procid_t> vidowner = globalvid2owner.get_cached(v);
    assert(vidowner.first);

    return rmi.remote_request(vidowner.second,
                              &distributed_graph<VertexData, EdgeData>::
                              in_edge_id_as_vec,
                              v);
  } // end of in edges

  /** \brief Return the edge ids of the edges leaving at v */
  dgraph_edge_list out_edge_ids(vertex_id_t v) const {
    boost::unordered_map<vertex_id_t, vertex_id_t>::const_iterator iter = 
      global2localvid.find(v);
    // if I have the vertex in my fragment
    // and if it is interior
    if (iter != global2localvid.end()) {
      vertex_id_t localvid = iter->second;
      if (localvid2owner[localvid]  == rmi.procid()) {
        return dgraph_edge_list(localstore.out_edge_ids(localvid), local2globaleid);
      }
    }
    // ok I need to construct a vector
    return dgraph_edge_list(out_edge_id_as_vec(v));
  } // end of out edges




  std::vector<edge_id_t> out_edge_id_as_vec(vertex_id_t v) const {
    boost::unordered_map<vertex_id_t, vertex_id_t>::const_iterator iter = 
      global2localvid.find(v);
    // if I have the vertex in my fragment
    // and if it is interior
    std::vector<edge_id_t> ret;
    if (iter != global2localvid.end()) {
      vertex_id_t localvid = iter->second;
      if (localvid2owner[localvid]  == rmi.procid()) {
        foreach(edge_id_t localeid, localstore.out_edge_ids(localvid)) {
          ret.push_back(local2globaleid[localeid]);
        }
        return ret;
      }
    }
    std::pair<bool, procid_t> vidowner = globalvid2owner.get_cached(v);
    assert(vidowner.first);

    return rmi.remote_request(vidowner.second,
                              &distributed_graph<VertexData, EdgeData>::
                              out_edge_id_as_vec,
                              v);
  } // end of in edges



  void print(std::ostream &out) const {
    for (size_t i = 0;i < localstore.num_edges(); ++i) {
      std::cout << local2globalvid[localstore.source(i)] << ", " 
                << local2globalvid[localstore.target(i)] << "\n";
    }
  }

  bool vertex_is_local(vertex_id_t vid) const{
    return global_vid_in_local_fragment(vid);
  }
  
  bool edge_is_local(edge_id_t eid) const{
    return global_eid_in_local_fragment(eid);
  }
  /**
   * Returns a reference to the edge data on the edge source->target
   * assertion failure if the edge is not within the current fragment
   */
  EdgeData& edge_data(vertex_id_t source, vertex_id_t target) {
    assert(global_vid_in_local_fragment(source));
    assert(global_vid_in_local_fragment(target));
    return localstore.edge_data(global2localvid.find(source)->second,
                                global2localvid.find(target)->second);
  }

  /**
   * Returns a constant reference to the edge data on the edge source->target
   * assertion failure if the edge is not within the current fragment
   */
  const EdgeData& edge_data(vertex_id_t source, vertex_id_t target) const{
    assert(global_vid_in_local_fragment(source));
    assert(global_vid_in_local_fragment(target));
    return localstore.edge_data(global2localvid.find(source)->second,
                                global2localvid.find(target)->second);
  }

  /**
   * Returns a reference to the edge data on the edge eid
   * assertion failure if the edge is not within the current fragment
   */
  EdgeData& edge_data(edge_id_t eid) {
    assert(global_eid_in_local_fragment(eid));
    return localstore.edge_data(global2localeid[eid]);
  }

  /**
   * Returns a constant reference to the edge data on the edge eid
   * assertion failure if the edge is not within the current fragment
   */
  const EdgeData& edge_data(edge_id_t eid) const{
    assert(global_eid_in_local_fragment(eid));
    return localstore.edge_data(global2localeid.find(eid)->second);
  }

  /**
   * Returns a reference to the vertex data on vertex vid
   * assertion failure if the vertex is not within the current fragment
   */
  VertexData& vertex_data(vertex_id_t vid) {
    assert(global_vid_in_local_fragment(vid));
    return localstore.vertex_data(global2localvid[vid]);
  }

  /**
   * Returns a constant reference to the vertex data on vertex vid
   * assertion failure if the vertex is not within the current fragment
   */
  const VertexData& vertex_data(vertex_id_t vid) const{
    assert(global_vid_in_local_fragment(vid));
    return localstore.vertex_data(global2localvid.find(vid)->second);
  }


  /**
   * Returns a copy of the edge data on the edge source->target
   * If the edge is not on this fragment, the request is sent
   * to a remote machine.
   */
  EdgeData get_edge_data_from_pair(vertex_id_t source, 
                                   vertex_id_t target) const {
    if (global_vid_in_local_fragment(source) && 
        global_vid_in_local_fragment(target)) {
      return edge_data(source, target);
    }
    else {
      std::pair<bool, procid_t> vidowner = 
        globalvid2owner.get_cached(target);
      assert(vidowner.first);

      return rmi.remote_request(vidowner.second,
                                &distributed_graph<VertexData,EdgeData>::
                                get_edge_data_from_pair,
                                source,
                                target);
    }
  }

  /**
   * Returns a copy of the edge data on the edge eid
   * If the edge is not on this fragment, the request is sent
   * to a remote machine.
   */
  EdgeData get_edge_data_from_eid(edge_id_t eid) const{
    if (global_eid_in_local_fragment(eid)) {
      return edge_data(eid);
    }
    else {
      ASSERT_MSG(edge_canonical_numbering == false,
                "Remote edge request impossible due to use of canonical edge numbering");

      std::pair<bool, procid_t> eidowner = globaleid2owner.get_cached(eid);
      assert(eidowner.first);

      return rmi.remote_request(eidowner.second,
                                &distributed_graph<VertexData,EdgeData>::
                                get_edge_data_from_eid,
                                eid);
    }
  }

  /**
   * Returns a copy of the edge data on the edge source->target
   * If the edge is not on this fragment, the request is sent
   * to a remote machine.
   */
  EdgeData get_edge_data(vertex_id_t source, vertex_id_t target) const {
    return get_edge_data_from_pair(source, target);
  }

  /**
   * Returns a copy of the edge data on the edge eid
   * If the edge is not on this fragment, the request is sent
   * to a remote machine.
   */
  EdgeData get_edge_data(edge_id_t eid) const{
    return get_edge_data_from_eid(eid);
  }

  /**
   * Returns a copy of the vertex data on the vertex vid
   * If the vertex is not on this fragment, the request is sent
   * to a remote machine.
   */
  VertexData get_vertex_data(vertex_id_t vid) const{
    if (global_vid_in_local_fragment(vid)) {
      return vertex_data(vid);
    }
    else {
      std::pair<bool, procid_t> vidowner = globalvid2owner.get_cached(vid);
      assert(vidowner.first);

      return rmi.remote_request(vidowner.second,
                                &distributed_graph<VertexData,EdgeData>::
                                get_vertex_data,
                                vid);
    }
  }



  /**
   * Sets the data on the edge source->target If the edge is not on
   * this fragment, the request is sent to a remote machine. If async
   * is true, the function returns immediately without waiting for
   * confirmation from the remote machine.
   */
  void set_edge_data_from_pair(vertex_id_t source, vertex_id_t target,
                              const EdgeData edata, bool async) {
    // sets must go straight to the owner
    boost::unordered_map<vertex_id_t, vertex_id_t>::const_iterator targetiter = 
      global2localvid.find(target);
    // if I own the target vertex, then I own the edge
    if (targetiter != global2localvid.end()) {
      if (localvid2owner[targetiter->second] == rmi.procid()) {
        edge_data(source, target) = edata;
        return;
      }
    }
    std::pair<bool, procid_t> vidowner = globalvid2owner.get_cached(target);
    assert(vidowner.first);

    if (async) {
      rmi.remote_call(vidowner.second,
                      &distributed_graph<VertexData,EdgeData>::
                      set_edge_data_from_pair,
                      source,
                      target,
                      edata);
    }
    else {
      rmi.remote_request(vidowner.second,
                         &distributed_graph<VertexData,EdgeData>::
                         set_edge_data_from_pair,
                         source,
                         target,
                         edata);
    }
  }

  /**
   * Sets the data on the edge eid
   * If the edge is not on this fragment, the request is sent
   * to a remote machine. If async is true, the function returns immediately
   * without waiting for confirmation from the remote machine.
   */
  void set_edge_data_from_eid(edge_id_t eid, 
                              const EdgeData edata, bool async){
    boost::unordered_map<edge_id_t, edge_id_t>::const_iterator eiditer = 
      global2localeid.find(eid);
    if (eiditer != global2localeid.end()) {
      // who owns the target of the edge?
      if (localvid2owner[localstore.target(eiditer->second)] == rmi.procid()) {
        // if I do. then I must own the edge.
        edge_data(eid) = edata;
        return;
      }
    }
    ASSERT_MSG(edge_canonical_numbering == false,
              "Remote edge request impossible due to use of canonical edge numbering");

    std::pair<bool, procid_t> eidowner = globaleid2owner.get_cached(eid);
    assert(eidowner.first);

    if (async) {
      rmi.remote_call(eidowner.second,
                      &distributed_graph<VertexData,EdgeData>::
                      set_edge_data_from_eid,
                      eid,
                      edata);
    }
    else {
      rmi.remote_request(eidowner.second,
                        &distributed_graph<VertexData,EdgeData>::
                         set_edge_data_from_eid,
                        eid,
                        edata);
    }
  }

  /**
   * Sets the data on the edge source->target
   * If the edge is not on this fragment, the request is sent
   * to a remote machine. This operation is performed synchronously.
   * It will wait for the remote machine to complete the modification before
   * returning control.
   */
  void set_edge_data(vertex_id_t source, vertex_id_t target, 
                     const EdgeData edata) {
    set_edge_data_from_pair(source, target, edata, false);
  }

  /**
   * Sets the data on the edge eid
   * If the edge is not on this fragment, the request is sent
   * to a remote machine. This operation is performed synchronously.
   * It will wait for the remote machine to complete the modification before
   * returning control.
   */
  void set_edge_data(edge_id_t eid, const EdgeData edata){
    set_edge_data_from_eid(eid, edata, false);
  }

  /**
   * Sets the data on the vertex vid
   * If the vertex is not on this fragment, the request is sent
   * to a remote machine. This operation is performed synchronously.
   * It will wait for the remote machine to complete the modification before
   * returning control.
   */
  void set_vertex_data(vertex_id_t vid, const VertexData vdata){
    if (global_vid_in_local_fragment(vid)) {
      vertex_data(vid) = vdata;
    }
    else {
      std::pair<bool, procid_t> vidowner = globalvid2owner.get_cached(vid);
      assert(vidowner.first);
      rmi.remote_request(vidowner.second,
                        &distributed_graph<VertexData,EdgeData>::set_vertex_data,
                        vdata);
    }
  }

  /**
   * Sets the data on the edge source->target
   * If the edge is not on this fragment, the request is sent
   * to a remote machine. This modification is performed asynchronously.
   */
  void set_edge_data_async(vertex_id_t source, 
                           vertex_id_t target, 
                           const EdgeData edata) {
    set_edge_data_from_pair(source, target, edata, true);
  }

  /**
   * Sets the data on the edge eid
   * If the edge is not on this fragment, the request is sent
   * to a remote machine. This modification is performed asynchronously.
   */
  void set_edge_data_async(edge_id_t eid, const EdgeData edata){
    set_edge_data_from_eid(eid, edata, true);
  }

  /**
   * Sets the data on the vertex vid.
   * If the vertex is not on this fragment, the request is sent
   * to a remote machine. This modification is performed asynchronously.
   */
  void set_vertex_data_async(vertex_id_t vid, const VertexData vdata){
    if (global_vid_in_local_fragment(vid)) {
      vertex_data(vid) = vdata;
    }
    else {
      std::pair<bool, procid_t> vidowner = 
        globalvid2owner.get_cached(vid);
      assert(vidowner.first);
      rmi.remote_call(vidowner.second,
                      &distributed_graph<VertexData,EdgeData>::
                      set_vertex_data_async,
                      vdata);
    }
  }

  /**
   * Gets a reference to the color on vertex vid.
   * Assertion failure if vid is not on this machine.
   */
  const vertex_color_type& color(vertex_id_t vid) {
    assert(global_vid_in_local_fragment(vid));
    return localstore.color(global2localvid[vid]);
  }

  /**
   * Gets a constant reference to the color on vertex vid.
   * Assertion failure if vid is not on this machine.
   */
  const vertex_color_type& color(vertex_id_t vid) const {
    assert(global_vid_in_local_fragment(vid));
    return localstore.color(global2localvid[vid]);
  }

  /**
   * Gets the color on vertex vid.
   * If the vertex is not on this fragment, the request is sent
   * to a remote machine.
   */
  vertex_color_type get_color(vertex_id_t vid) const{
    if (global_vid_in_local_fragment(vid)) {
      return localstore.color(global2localvid[vid]);
    }
    else {
      std::pair<bool, procid_t> vidowner = globalvid2owner.get_cached(vid);
      assert(vidowner.first);
      return rmi.remote_request(vidowner.second,
                                &distributed_graph<VertexData,EdgeData>::get_color,
                                vid);
    }
  }

  /**
   * Sets the color on vertex vid.
   * If the vertex is not on this fragment, the request is sent
   * to a remote machine. This operation is performed synchronously.
   * It will wait for the remote machine to complete the modification before
   * returning control.
   */
  void set_color(vertex_id_t vid, vertex_color_type color) const{
    if (global_vid_in_local_fragment(vid)) {
      localstore.color(global2localvid[vid]) = color;
    }
    else {
      std::pair<bool, procid_t> vidowner = globalvid2owner.get_cached(vid);
      assert(vidowner.first);
      return rmi.remote_request(vidowner.second,
                                &distributed_graph<VertexData,EdgeData>::set_color,
                                vid,
                                color);
    }
  }

  /**
   * Sets the color on vertex vid.
   * If the vertex is not on this fragment, the request is sent
   * to a remote machine. This operation is performed asynchronously.
   */
  void set_color_async(vertex_id_t vid, vertex_color_type color) const{
    if (global_vid_in_local_fragment(vid)) {
      localstore.color(global2localvid[vid]) = color;
    }
    else {
      std::pair<bool, procid_t> vidowner = globalvid2owner.get_cached(vid);
      assert(vidowner.first);
      return rmi.remote_call(vidowner.second,
                            &distributed_graph<VertexData,EdgeData>::set_color_async,
                            vid,
                            color);
    }
  }


  bool is_ghost(vertex_id_t vid) {
    return localvid2owner[global2localvid[vid]] != rmi.procid();
  }

  /**
   * In a synchronous way, synchronize the data on vertex with global id vid
   * vid must be a ghost
   */
  void synchronize_vertex(vertex_id_t vid, bool async = false) {
    vertex_id_t localvid = global2localvid[vid];
    if (is_ghost(vid)) {
      vertex_conditional_store out;
      out.hasdata = localstore.vertex_modified(localvid);
      if (out.hasdata) out.data.first = localstore.vertex_data(localvid);
      if (async == false) {
        vertex_conditional_store v = rmi.remote_request(localvid2owner[localvid],
                                                        &distributed_graph<VertexData, EdgeData>::get_vertex_if_version_less_than,
                                                        vid,
                                                        localstore.vertex_version(localvid),
                                                        out);
      }
      else {
        vertex_conditional_store v = rmi.remote_call(localvid2owner[localvid],
                                                     rmi.procid(),
                                                     &distributed_graph<VertexData, EdgeData>::async_get_vertex_if_version_less_than,
                                                     vid,
                                                     localstore.vertex_version(localvid),
                                                     out);
      }
      if (v.hasdata) {
        localstore.vertex_data(localvid) = v.data.first;
        localstore.vertex_version(localvid) = v.data.second;
      }
    }
  }

  /**
   * In a synchronous way, synchronize the data on edge with global id eid
   * target of edge must be a ghost
   */
  void synchronize_edge(edge_id_t eid, bool async = false) {

    if (!edge_canonical_numbering) {
      edge_id_t localeid = eid;

      edge_conditional_store out;
      out.hasdata = localstore.edge_modified(localeid);
      if (out.hasdata) out.data.first = localstore.edge_data(localeid);

      if (localvid2owner[localstore.target(localeid)] != rmi.procid()) {
        if (async == false) {
          edge_conditional_store e = rmi.remote_request(localvid2owner[localstore.target(localeid)],
                                                      &distributed_graph<VertexData, EdgeData>::get_edge_if_version_less_than,
                                                      eid,
                                                      localstore.vertex_version(localeid),
                                                      out);
        }
        else {
          edge_conditional_store e = rmi.remote_call(localvid2owner[localstore.target(localeid)],
                                                     rmi.procid(),
                                                     &distributed_graph<VertexData, EdgeData>::async_get_edge_if_version_less_than,
                                                     eid,
                                                     localstore.vertex_version(localeid),
                                                     out);
        }
        if (e.hasdata) {
          localstore.edge_data(localeid) = e.data.first;
          localstore.edge_version(localeid) = e.data.second;
        }
      }
    }
    else {
      edge_id_t localeid = eid;
      vertex_id_t localtargetvid = localstore.target(localeid);
      vertex_id_t targetvid = local2globalvid[localtargetvid];
      vertex_id_t localsourcevid = localstore.source(localeid);
      vertex_id_t sourcevid = local2globalvid[localsourcevid];

      if (is_ghost(targetvid)) {

        edge_conditional_store out;
        out.hasdata = localstore.edge_modified(localeid);
        if (out.hasdata) out.data.first = localstore.edge_data(localeid);
        if (async == false) {
          edge_conditional_store e = rmi.remote_request(localvid2owner[localtargetvid],
                                                        &distributed_graph<VertexData, EdgeData>::get_edge_if_version_less_than2,
                                                        sourcevid,
                                                        targetvid,
                                                        localstore.vertex_version(localeid),
                                                        out);
        }
        else {
          edge_conditional_store e = rmi.remote_call(localvid2owner[localtargetvid],
                                                     rmi.procid(),
                                                     &distributed_graph<VertexData, EdgeData>::async_get_edge_if_version_less_than2,
                                                     sourcevid,
                                                     targetvid,
                                                     localstore.vertex_version(localeid),
                                                     out);
        }
        if (e.hasdata) {
          localstore.edge_data(localeid) = e.data.first;
          localstore.edge_version(localeid) = e.data.second;
        }
      }
    }
  }



  

  /**
   * In a synchronous way, synchronize the entire scope for vertex vid
   */
  void synchronize_scope(vertex_id_t vid) {
    if (edge_canonical_numbering) {
      // use 2
      vertex_id_t localvid = global2localvid[vid];
      std::map<procid_t, >
      foreach(edge_id_t localineid, localstore.in_edge_ids(localvid)) {
        if (
      }
    }
  }



  /**
   * In a asynchronous way, synchronize the entire scope for vertex vid
   */
  void async_synchronize_scope(vertex_id_t vid) {

  }

  /**
   * Waits for all asynchronous data synchronizations to complete
   */
  void wait_for_all_async_syncs() {
    while(pending_async_updates.value != 0) sched_yield();
  }



  template <typename DataType>
  struct conditional_store{
    bool hasdata;
    DataType data;
    void save(oarchive &oarc) {
      oarc << hasdata;
      if (hasdata) oarc << data;
    }
    void load(iarchive &iarc) {
      iarc >>  hasdata;
      if (hasdata) iarc >> data;
    }
  };
  
 private:
  /// RMI object
  mutable dc_dist_object<distributed_graph<VertexData, EdgeData> > rmi;
  
  /** Protects structural modifications of the graph.
   * Modifications to the data store and to the local<->global mappings
   * must lock this.
   */
  mutex alldatalock;
  
  /// stores the local fragment of the graph
  dist_graph_impl::graph_local_store<VertexData, EdgeData> localstore;


  /** all the mappings requried to move from global to local vid/eids
   *  We only store mappings if the vid/eid is in the local fragment
   */
  boost::unordered_map<vertex_id_t, vertex_id_t> global2localvid;
  std::vector<vertex_id_t> local2globalvid;
  boost::unordered_map<edge_id_t, edge_id_t> global2localeid;
  std::vector<edge_id_t> local2globaleid;
   
  /** To avoid requiring O(V) storage on each maching, the 
   * global_vid -> owner mapping cannot be stored in its entirely locally
   * instead, we store it in a DHT. \see globaleid2owner
   */
  caching_dht<vertex_id_t, procid_t> globalvid2owner;
  
  /** To avoid requiring O(E) storage on each maching, the 
   * global_eid -> owner mapping cannot be stored in its entirely locally
   * instead, we store it in a DHT \see globalvid2owner
   */
  caching_dht<edge_id_t, procid_t> globaleid2owner;
  bool edge_canonical_numbering;
  
  /** This provides a fast mapping from the local vids in the fragment
   * to its owner. Since this operation is quite frequently needed.
   */
  std::vector<procid_t> localvid2owner;
  
  /**
   * The number of vertices and edges in the entire graph so far.
   * Currently only consistent on machine 0 since machine 0 manages 
   * the allocation of global VIDs and local VIDs.
   */
  size_t numglobalverts, numglobaledges;

  atomic<size_t> pending_async_updates;



  typedef conditional_store<std::pair<VertexData, uint64_t> >  vertex_conditional_store;
  typedef conditional_store<std::pair<EdgeData, uint64_t> >  edge_conditional_store;
  
  
  /**
   * Returns true if the global vid is in the local fragment
   * This is not synchronized. Caller must lock if there is a risk
   * of the structure changing while this check is performed.
   */
  bool global_vid_in_local_fragment(vertex_id_t globalvid) const{
    // easiest way to check is to see if it is in the global2localvid mapping
    return global2localvid.find(globalvid) != global2localvid.end();
  }
  
  /**
   * Returns true if the global eid is in the local fragment
   * This is not synchronized. Caller must lock if there is a risk
   * of the structure changing while this check is performed.
   */
  bool global_eid_in_local_fragment(edge_id_t globaleid) const{
    // easiest way to check is to see if it is in the global2localvid mapping
    return global2localeid.find(globaleid) != global2localeid.end();
  }
  
  
  /**
   * From the atoms listed in the atom index file, construct the local store
   * using all the atoms in the current partition.
   */
  void construct_local_fragment(const atom_index_file &atomindex,
                                std::vector<std::vector<size_t> > partitiontoatom,
                                size_t curpartition) {
    // first make a map mapping atoms to machines
    // we will need this later
    std::vector<procid_t> atom2machine;
    for (size_t i = 0 ;i< partitiontoatom.size(); ++i) {
      for (size_t j = 0 ; j < partitiontoatom[i].size(); ++j) {
        if (atom2machine.size() <= partitiontoatom[i][j]) 
          atom2machine.resize(partitiontoatom[i][j] + 1);
        atom2machine[partitiontoatom[i][j]] = i;
      }
    }

    
    
    // the atomfiles for the local fragment
    std::vector<atom_file<VertexData, EdgeData>* > atomfiles;
    // for convenience take a reference to the list of atoms in this partition
    std::vector<size_t>& atoms_in_curpart = partitiontoatom[curpartition];

    logger(LOG_INFO, "Loading ID maps");
    // create the atom file readers.
    // and load the vid / eid mappings
    atomfiles.resize(atoms_in_curpart.size());
    for (size_t i = 0;i < atoms_in_curpart.size(); ++i) {
      atomfiles[i] = new atom_file<VertexData, EdgeData>;
      atomfiles[i]->input_filename(atomindex.atoms[atoms_in_curpart[i]].protocol,
                                atomindex.atoms[atoms_in_curpart[i]].file);
      atomfiles[i]->load_id_maps();
    }

    logger(LOG_INFO, "Generating mappings");

    edge_canonical_numbering = (atomfiles[0]->globaleids().size() == 0);
    if (edge_canonical_numbering) {
      logger(LOG_WARNING, "Edge Canonical Numbering used. Edge IDs are only locally valid");
    }
    // Lets first construct the global/local vid/eid mappings by merging
    // the mappings in each atom
    // cat all the globalvids and globaleids into a single big list
    // and sort it
    for (size_t i = 0;i < atomfiles.size(); ++i) {
      std::copy(atomfiles[i]->globalvids().begin(), 
                atomfiles[i]->globalvids().end(),
                std::back_inserter(local2globalvid));
    }
    
    // Find only unique occurances of each vertex, by sorting, unique,
    // and resize
    std::sort(local2globalvid.begin(), local2globalvid.end());
    std::vector<vertex_id_t>::iterator uviter = 
      std::unique(local2globalvid.begin(), local2globalvid.end());
    local2globalvid.resize(uviter - local2globalvid.begin());
    
    // construct localvid2owner
    localvid2owner.resize(local2globalvid.size());
    //construct the reverse maps
    for (size_t i = 0; i < local2globalvid.size(); ++i) {
      global2localvid[local2globalvid[i]] = i;
    }




      // repeat for edges if globaleids are available

    if (!edge_canonical_numbering) {
      for (size_t i = 0;i < atomfiles.size(); ++i) {
        std::copy(atomfiles[i]->globaleids().begin(),
                  atomfiles[i]->globaleids().end(),
                  std::back_inserter(local2globaleid));
      }
      // Find only unique occurances of each edge, by sorting, unique,
      // and resize
      std::sort(local2globaleid.begin(), local2globaleid.end());
      std::vector<edge_id_t>::iterator ueiter =
        std::unique(local2globaleid.begin(), local2globaleid.end());

      local2globaleid.resize(ueiter - local2globaleid.begin());

      for (size_t i = 0; i < local2globaleid.size(); ++i) {
        global2localeid[local2globaleid[i]] = i;
      }
    }




    logger(LOG_INFO, "Loading Structure");
    std::map<std::pair<vertex_id_t, vertex_id_t>, edge_id_t> canonical_numbering;
    for (size_t i = 0;i < atomfiles.size(); ++i) {
      atomfiles[i]->load_structure();
      for (size_t j = 0;j < atomfiles[i]->edge_src_dest().size(); ++j) {
        if (canonical_numbering.find(atomfiles[i]->edge_src_dest()[j]) == canonical_numbering.end()) {
          size_t newid = canonical_numbering.size();
          canonical_numbering[atomfiles[i]->edge_src_dest()[j]] = newid;
        }
      }
    }

    if (edge_canonical_numbering) {
      // make a fake local2globaleid and global2localeid
      local2globaleid.resize(canonical_numbering.size());
      for (size_t i = 0;i < local2globaleid.size(); ++i) {
        local2globaleid[i] = i;
        global2localeid[i] = i;
      }
    }

    
    logger(LOG_INFO, "Creating mmap store");
    // now lets construct the graph structure
    size_t nedges_to_create = std::max(canonical_numbering.size(), local2globaleid.size());
    localstore.create_store(local2globalvid.size(), nedges_to_create,
                            "vdata." + tostr(curpartition),
                            "edata." + tostr(curpartition));

    logger(LOG_INFO, "Loading Structure");
    // load the graph structure
    std::vector<bool> eidloaded(nedges_to_create, false);
        

    for (size_t i = 0;i < atomfiles.size(); ++i) {
      // iterate through all the edges in this atom
      for (size_t j = 0;j < atomfiles[i]->edge_src_dest().size(); ++j) {
        // convert from the atom's local eid, to the global eid, then
        // to the fragment localeid
        edge_id_t localeid;
        if (!edge_canonical_numbering) {
          localeid = global2localeid[atomfiles[i]->globaleids()[j]];
        }
        else {
          localeid = canonical_numbering[atomfiles[i]->edge_src_dest()[j]];
        }
        if (eidloaded[localeid] == false) {
          std::pair<vertex_id_t, vertex_id_t> srcdest = 
            atomfiles[i]->edge_src_dest()[j];
          vertex_id_t sourcevid = 
            global2localvid[atomfiles[i]->globalvids()[srcdest.first]];
          vertex_id_t destvid = 
            global2localvid[atomfiles[i]->globalvids()[srcdest.second]];
          localstore.add_edge(localeid, sourcevid, destvid);
          eidloaded[localeid] = true;
        }
      }
      
      // set the color and localvid2owner mappings
      for (size_t j = 0; j < atomfiles[i]->vcolor().size(); ++j) {
        // convert from the atom's local vid, to the global vid, then
        // to the fragment localvid
        vertex_id_t globalvid = atomfiles[i]->globalvids()[j];
        vertex_id_t localvid = global2localvid[globalvid];

        localvid2owner[localvid] = atom2machine[atomfiles[i]->atom()[j]];
        localstore.color(localvid) = atomfiles[i]->vcolor()[j];
        // if I own this vertex, set the global ownership to me
        if (localvid2owner[localvid] == rmi.procid()) {
          globalvid2owner.set(globalvid, rmi.procid());
        }
      }
    }
    
    if (!edge_canonical_numbering) {
      logger(LOG_INFO, "Set up global eid table");
      // unfortunately, I need one more pass here to set ownership of
      // all the edgeids I can only do this after all the vid ownerships
      // are set
      for (size_t i = 0;i < atomfiles.size(); ++i) {
        for (size_t j = 0;j < atomfiles[i]->edge_src_dest().size(); ++j) {
          edge_id_t globaleid = atomfiles[i]->globaleids()[j];
          vertex_id_t targetlocalvid = atomfiles[i]->edge_src_dest()[j].second;
          // do I own it?
          if (localvid2owner[targetlocalvid] == rmi.procid()) {
            // then I own this edge
            globaleid2owner.set(globaleid, rmi.procid());
          }
        }
      }
    }
    else {
      logger(LOG_INFO, "edge canonical numbering used. global eid table not needed");
    }
    
    logger(LOG_INFO, "Loading data");
    // done! structure constructed!  now for the data!  load atoms one
    // at a time, don't keep more than one atom in memor at any one
    // time
    for (size_t i = 0;i < atomfiles.size(); ++i) {
      atomfiles[i]->load_all();
      for (size_t j = 0; j < atomfiles[i]->vdata().size(); ++j) {
        // convert from the atom's local vid, to the global vid, then
        // to the fragment localvi
        size_t localvid = global2localvid[atomfiles[i]->globalvids()[j]];
        localstore.vertex_data(localvid) = atomfiles[i]->vdata()[j];
        localstore.vertex_version(localvid) = 0;
      }
      for (size_t j = 0; j < atomfiles[i]->edata().size(); ++j) {
        // convert from the atom's local vid, to the global vid, then
        // to the fragment localvi
        size_t localeid = global2localeid[atomfiles[i]->globaleids()[j]];
        localstore.edge_data(localeid) = atomfiles[i]->edata()[j];
        localstore.edge_version(localeid) = 0;
      }
      atomfiles[i]->clear();
      delete atomfiles[i];
    }
    // flush the store
    logger(LOG_INFO, "Finalize");
    localstore.finalize();
    logger(LOG_INFO, "Flush");
    localstore.flush();
    logger(LOG_INFO, "Prefetch computation");
    localstore.compute_minimal_prefetch();
    logger(LOG_INFO, "Load complete.");
    rmi.comm_barrier();
  }

  vertex_conditional_store get_vertex_if_version_less_than(vertex_id_t vid,
                                                           uint64_t  vertexversion,
                                                           vertex_conditional_store vdata) {
    vertex_conditional_store ret;
    size_t localvid = global2localvid[vid];
    uint64_t local_vertex_version = localstore.vertex_version(localvid);
    if (local_vertex_version  > vertexversion) {
      ret.hasdata = true;
      ret.data.first = localstore.vertex_data(localvid);
      ret.data.second = local_vertex_version;
    }
    else if (local_vertex_version < vertexversion) {
      assert(vdata.hasdata);
      localstore.vertex_data(localvid) = vdata.data.first;
      localstore.set_vertex_version(localvid, vertexversion);
      ret.hasdata = false;
    }
    else {
      ret.hasdata = false;
    }
    return ret;
  }

  edge_conditional_store get_edge_if_version_less_than(edge_id_t eid, uint64_t  edgeversion,
                                                       edge_conditional_store edata) {
    edge_conditional_store ret;
    size_t localeid = global2localeid[eid];
    uint64_t  local_edge_version = localstore.edge_version(localeid);
    if (local_edge_version > edgeversion) {
      ret.hasdata = true;
      ret.data.first = localstore.edge_data(localeid);
      ret.data.second = local_edge_version;
    }
    else if (local_edge_version < edgeversion) {
      assert(edata.hasdata);
      localstore.edge_data(localeid) = edata.data.first;
      localstore.set_edge_version(localeid, edgeversion);
      ret.hasdata = false;
    }
    else {
      ret.hasdata = false;
    }
    return ret;
  }

  edge_conditional_store get_edge_if_version_less_than2(vertex_id_t source,
                                                        vertex_id_t target,
                                                        uint64_t  edgeversion,
                                                        edge_conditional_store edata) {
    edge_conditional_store ret;
    size_t localsource = global2localvid[source];
    size_t localtarget = global2localvid[target];
    std::pair<bool, edge_id_t> findret = localstore.find(localsource, localtarget);
    assert(findret.first);
    edge_id_t localeid = findret.second;
    
    uint64_t  local_edge_version = localstore.edge_version(localeid);
    if (local_edge_version > edgeversion) {
      ret.hasdata = true;
      ret.data.first = localstore.edge_data(localeid);
      ret.data.second = local_edge_version;
    }
    else if (local_edge_version < edgeversion) {
      assert(edata.hasdata);
      localstore.edge_data(localeid) = edata.data.first;
      localstore.set_edge_version(localeid, edgeversion);
      ret.hasdata = false;
    }
    else {
      ret.hasdata = false;
    }
    return ret;
  }



  void async_get_vertex_if_version_less_than(procid_t srcproc, vertex_id_t vid, uint64_t  vertexversion) {
    rmi.remote_call(srcproc,
                    &distributed_graph<VertexData, EdgeData>::reply_vertex_data_and_version,
                    vid,
                    get_vertex_if_version_less_than(vid, vertexversion));
  }

  void async_get_edge_if_version_less_than(procid_t srcproc, edge_id_t eid, uint64_t  edgeversion) {
    rmi.remote_call(srcproc,
                    &distributed_graph<VertexData, EdgeData>::reply_edge_data_and_version,
                    eid,
                    get_edge_if_version_less_than(eid, edgeversion));
  }

  void async_get_edge_if_version_less_than2(procid_t srcproc, vertex_id_t source, vertex_id_t target, uint64_t  edgeversion) {
    rmi.remote_call(srcproc,
                    &distributed_graph<VertexData, EdgeData>::reply_edge_data_and_version2,
                    source,
                    target,
                    get_edge_if_version_less_than2(source, target, edgeversion));
  }

  struct block_synchronize_request {
    std::vector<vertex_id_t> vid;
    std::vector<uint64_t > vidversion;
    std::vector<vertex_conditional_store> vstore;
    std::vector<edge_id_t> eid;
    std::vector<uint64_t > edgeversion;
    std::vector<edge_conditional_store> estore;
    void save(oarchive &oarc) {
      oarc << vid << vidversion << vstore
           << eid << edgeversion << estore;
    }

    void load(iarchive &iarc) {
      oarc >> vid >> vidversion >> vstore
           >> eid >> edgeversion >> estore;
    }
  };

  block_synchronize_request& get_alot(procid_t srcproc,
                                     block_synchronize_request &request) {
    std::vector<vertex_conditional_store> vresponse(request.vid.size());
    std::vector<edge_conditional_store> eresponse(request.eid.size());
    for (size_t i = 0;i < request.vid.size(); ++i) {
      request.vstore[i] = get_vertex_if_version_less_than(request.vid[i], request.vidversion[i], request.vstore[i]);
    }
    for (size_t i = 0;i < request.eid.size(); ++i) {
      request.estore[i] = get_edge_if_version_less_than(request.eid[i], request.edgeversion[i], request.estore[i]);
    }
    request.vidversion.clear();
    request.edgeversion.clear();

    return request;
  }


  void async_get_alot(procid_t srcproc,
                      block_synchronize_request &request) {
    get_alot(srcproc, request);
    rmi.remote_call(srcproc,
                    &distributed_graph<VertexData, EdgeData>::reply_alot,
                    request);
  }


  struct block_synchronize_request2 {
    std::vector<vertex_id_t> vid;
    std::vector<uint64_t > vidversion;
    std::vector<vertex_conditional_store> vstore;
    std::vector<std::pair<vertex_id_t, vertex_id_t> > srcdest;
    std::vector<uint64_t > edgeversion;
    std::vector<edge_conditional_store> estore;
    void save(oarchive &oarc) {
      oarc << vid << vidversion << vstore
           << srcdest << edgeversion << estore;
    }

    void load(iarchive &iarc) {
      oarc >> vid >> vidversion >> vstore
           >> srcdest >> edgeversion >> estore;
    }
  };

  block_synchronize_request2& get_alot2(procid_t srcproc,
                                        block_synchronize_request2 &request) {
    std::vector<vertex_conditional_store> vresponse(request.vid.size());
    std::vector<edge_conditional_store> eresponse(request.srcdest.size());
    for (size_t i = 0;i < request.vid.size(); ++i) {
      request.vstore[i] = get_vertex_if_version_less_than(request.vid[i], request.vidversion[i], request.vstore[i]);
    }
    for (size_t i = 0;i < request.srcdest.size(); ++i) {
      request.estore[i] = get_edge_if_version_less_than2(request.srcdest[i].first, request.srcdest[i].second, request.edgeversion[i], request.estore[i]);
    }
    request.vidversion.clear();
    request.edgeversion.clear();
    return request;
  }
  
  void async_get_alot2(procid_t srcproc,
                       block_synchronize_request2 &request) {
    get_alot2(srcproc, request);
    rmi.remote_call(srcproc,
                    &distributed_graph<VertexData, EdgeData>::reply_alot2,
                    request);
  }

  void reply_vertex_data_and_version(vertex_id_t vid, vertex_conditional_store &vstore) {
    update_vertex_data_and_version(vid, vstore);
    pending_async_updates.dec();
  }

  void reply_edge_data_and_version(edge_id_t eid, edge_conditional_store &estore) {
    update_edge_data_and_version(eid, estore);
    pending_async_updates.dec();
  }

  void reply_edge_data_and_version2(vertex_id_t source, vertex_id_t target, edge_conditional_store &estore) {
    update_edge_data_and_version2(source, target, estore);
    pending_async_updates.dec();
  }


  void update_vertex_data_and_version(vertex_id_t vid, vertex_conditional_store &vstore) {
    if (vstore.hasdata) {
      vertex_id_t localvid = global2localvid[vid];
      localstore.vertex_data(localvid) = vstore.data.first;
      localstore.vertex_version(localvid) = vstore.data.second;
    }
  }

  void update_edge_data_and_version(edge_id_t eid, edge_conditional_store &estore) {
    if (estore.hasdata) {
      edge_id_t localeid = global2localeid[eid];
      localstore.edge_data(localeid) = estore.data.first;
      localstore.edge_version(localeid) = estore.data.second;
    }
  }

  void update_edge_data_and_version2(vertex_id_t source, vertex_id_t target, edge_conditional_store &estore) {
    if (estore.hasdata) {
      vertex_id_t localsourcevid = global2localvid[source];
      vertex_id_t localtargetvid = global2localvid[target];
      std::pair<bool, edge_id_t> findret = localstore.find(localsourcevid, localtargetvid);
      assert(findret.first);
      localstore.edge_data(findret.second) = estore.data.first;
      localstore.edge_version(findret.second) = estore.data.second;
    }
  }

  void reply_alot(block_synchronize_request &request) {
    for (size_t i = 0;i < request.vid.size(); ++i) {
      update_vertex_data_and_version(request.vid[i], request.vstore[i]);
    }

    for (size_t i = 0;i < request.eid.size(); ++i) {
      update_edge_data_and_version(request.eid[i], request.estore[i]);
    }
    pending_async_updates.dec();
  }

  void reply_alot2(block_synchronize_request2 &request) {
    for (size_t i = 0;i < request.vid.size(); ++i) {
      update_vertex_data_and_version(request.vid[i], request.vstore[i]);
    }

    for (size_t i = 0;i < request.srcdest.size(); ++i) {
      update_edge_data_and_version2(request.srcdest[i].first, request.srcdest[i].second, request.estore[i]);
    }
    pending_async_updates.dec();
  }
};

template<typename VertexData, typename EdgeData>
std::ostream& operator<<(std::ostream& out,
                           const distributed_graph<VertexData, EdgeData>& graph) {
  graph.print(out);
  return out;
}


}

#include <graphlab/macros_undef.hpp>
#endif
