// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NINJA_GRAPH_H_
#define NINJA_GRAPH_H_

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "dyndep.h"
#include "eval_env.h"
#include "timestamp.h"
#include "util.h"

struct BuildLog;
struct DepfileParserOptions;
struct DiskInterface;
struct DepsLog;
struct Edge;
struct Node;
struct Pool;
struct State;

/// Information about a node in the dependency graph: the file, whether
/// it's dirty, mtime, etc.
struct Node {
  Node(const std::string& path, uint64_t slash_bits)
      : path_(path), slash_bits_(slash_bits), mtime_(-1), exists_(ExistenceStatusUnknown), dirty_(false),
        dyndep_pending_(false), in_edge_(NULL), id_(-1) {}

  /// Return false on error.
  bool Stat(DiskInterface* disk_interface, std::string* err);

  /// If the file doesn't exist, set the mtime_ from its dependencies
  void UpdatePhonyMtime(TimeStamp mtime);

  /// Return false on error.
  bool StatIfNecessary(DiskInterface* disk_interface, std::string* err) {
    if (status_known())
      return true;
    return Stat(disk_interface, err);
  }

  /// Mark as not-yet-stat()ed and not dirty.
  void ResetState() {
    mtime_ = -1;
    exists_ = ExistenceStatusUnknown;
    dirty_ = false;
  }

  /// Mark the Node as already-stat()ed and missing.
  void MarkMissing() {
    if (mtime_ == -1) {
      mtime_ = 0;
    }
    exists_ = ExistenceStatusMissing;
  }

  bool exists() const { return exists_ == ExistenceStatusExists; }

  bool status_known() const { return exists_ != ExistenceStatusUnknown; }

  const std::string& path() const { return path_; }
  /// Get |path()| but use slash_bits to convert back to original slash styles.
  std::string PathDecanonicalized() const { return PathDecanonicalized(path_, slash_bits_); }
  static std::string PathDecanonicalized(const std::string& path, uint64_t slash_bits);
  uint64_t slash_bits() const { return slash_bits_; }

  TimeStamp mtime() const { return mtime_; }

  bool dirty() const { return dirty_; }
  void set_dirty(bool dirty) { dirty_ = dirty; }
  void MarkDirty() { dirty_ = true; }

  bool dyndep_pending() const { return dyndep_pending_; }
  void set_dyndep_pending(bool pending) { dyndep_pending_ = pending; }

  Edge* in_edge() const { return in_edge_; }
  void set_in_edge(Edge* edge) { in_edge_ = edge; }

  /// Indicates whether this node was generated from a depfile or dyndep file,
  /// instead of being a regular input or output from the Ninja manifest.
  bool generated_by_dep_loader() const { return generated_by_dep_loader_; }

  void set_generated_by_dep_loader(bool value) { generated_by_dep_loader_ = value; }

  int id() const { return id_; }
  void set_id(int id) { id_ = id; }

  const std::vector<Edge*>& out_edges() const { return out_edges_; }
  const std::vector<Edge*>& validation_out_edges() const { return validation_out_edges_; }
  void AddOutEdge(Edge* edge) { out_edges_.push_back(edge); }
  void AddValidationOutEdge(Edge* edge) { validation_out_edges_.push_back(edge); }

  void Dump(const char* prefix = "") const;

 private:
  std::string path_;

  /// Set bits starting from lowest for backslashes that were normalized to
  /// forward slashes by CanonicalizePath. See |PathDecanonicalized|.
  uint64_t slash_bits_;

  /// Possible values of mtime_:
  ///   -1: file hasn't been examined
  ///   0:  we looked, and file doesn't exist
  ///   >0: actual file's mtime, or the latest mtime of its dependencies if it
  ///   doesn't exist
  TimeStamp mtime_;

  enum ExistenceStatus {
    /// The file hasn't been examined.
    ExistenceStatusUnknown,
    /// The file doesn't exist. mtime_ will be the latest mtime of its
    /// dependencies.
    ExistenceStatusMissing,
    /// The path is an actual file. mtime_ will be the file's mtime.
    ExistenceStatusExists
  };
  ExistenceStatus exists_;

  /// Dirty is true when the underlying file is out-of-date.
  /// But note that Edge::outputs_ready_ is also used in judging which
  /// edges to build.
  bool dirty_;

  /// Store whether dyndep information is expected from this node but
  /// has not yet been loaded.
  bool dyndep_pending_;

  /// Set to true when this node comes from a depfile, a dyndep file or the
  /// deps log. If it does not have a producing edge, the build should not
  /// abort if it is missing (as for regular source inputs). By default
  /// all nodes have this flag set to true, since the deps and build logs
  /// can be loaded before the manifest.
  bool generated_by_dep_loader_ = true;

  /// The Edge that produces this Node, or NULL when there is no
  /// known edge to produce it.
  Edge* in_edge_;

  /// All Edges that use this Node as an input.
  std::vector<Edge*> out_edges_;

  /// All Edges that use this Node as a validation.
  std::vector<Edge*> validation_out_edges_;

  /// A dense integer id for the node, assigned and used by DepsLog.
  int id_;
};

/// An edge in the dependency graph; links between Nodes using Rules.
struct Edge {
  enum VisitMark { VisitNone, VisitInStack, VisitDone };

  Edge()
      : rule_(NULL), pool_(NULL), dyndep_(NULL), env_(NULL), mark_(VisitNone), id_(0), outputs_ready_(false),
        deps_loaded_(false), deps_missing_(false), generated_by_dep_loader_(false), command_start_time_(0),
        implicit_deps_(0), order_only_deps_(0), implicit_outs_(0) {}

  /// Return true if all inputs' in-edges are ready.
  bool AllInputsReady() const;

  /// Expand all variables in a command and return it as a string.
  /// If incl_rsp_file is enabled, the string will also contain the
  /// full contents of a response file (if applicable)
  std::string EvaluateCommand(bool incl_rsp_file = false) const;

  /// Returns the shell-escaped value of |key|.
  std::string GetBinding(const std::string& key) const;
  bool GetBindingBool(const std::string& key) const;

  /// Like GetBinding("depfile"), but without shell escaping.
  std::string GetUnescapedDepfile() const;
  /// Like GetBinding("dyndep"), but without shell escaping.
  std::string GetUnescapedDyndep() const;
  /// Like GetBinding("rspfile"), but without shell escaping.
  std::string GetUnescapedRspfile() const;

  void Dump(const char* prefix = "") const;

  // Append all edge explicit inputs to |*out|. Possibly with shell escaping.
  void CollectInputs(bool shell_escape, std::vector<std::string>* out) const;

  const Rule* rule_;
  Pool* pool_;
  std::vector<Node*> inputs_;
  std::vector<Node*> outputs_;
  std::vector<Node*> validations_;
  Node* dyndep_;
  BindingEnv* env_;
  VisitMark mark_;
  size_t id_;
  bool outputs_ready_;
  bool deps_loaded_;
  bool deps_missing_;
  bool generated_by_dep_loader_;
  TimeStamp command_start_time_;

  const Rule& rule() const { return *rule_; }
  Pool* pool() const { return pool_; }
  int weight() const { return 1; }
  bool outputs_ready() const { return outputs_ready_; }

  // There are three types of inputs.
  // 1) explicit deps, which show up as $in on the command line;
  // 2) implicit deps, which the target depends on implicitly (e.g. C headers),
  //                   and changes in them cause the target to rebuild;
  // 3) order-only deps, which are needed before the target builds but which
  //                     don't cause the target to rebuild.
  // These are stored in inputs_ in that order, and we keep counts of
  // #2 and #3 when we need to access the various subsets.
  int implicit_deps_;
  int order_only_deps_;
  bool is_implicit(size_t index) {
    return index >= inputs_.size() - order_only_deps_ - implicit_deps_ && !is_order_only(index);
  }
  bool is_order_only(size_t index) { return index >= inputs_.size() - order_only_deps_; }

  // There are two types of outputs.
  // 1) explicit outs, which show up as $out on the command line;
  // 2) implicit outs, which the target generates but are not part of $out.
  // These are stored in outputs_ in that order, and we keep a count of
  // #2 to use when we need to access the various subsets.
  int implicit_outs_;
  bool is_implicit_out(size_t index) const { return index >= outputs_.size() - implicit_outs_; }

  bool is_phony() const;
  bool use_console() const;
  bool maybe_phonycycle_diagnostic() const;
};

struct EdgeCmp {
  bool operator()(const Edge* a, const Edge* b) const { return a->id_ < b->id_; }
};

typedef std::set<Edge*, EdgeCmp> EdgeSet;

/// ImplicitDepLoader loads implicit dependencies, as referenced via the
/// "depfile" attribute in build files.
struct ImplicitDepLoader {
  ImplicitDepLoader(State* state, DepsLog* deps_log, DiskInterface* disk_interface,
                    DepfileParserOptions const* depfile_parser_options)
      : state_(state), disk_interface_(disk_interface), deps_log_(deps_log),
        depfile_parser_options_(depfile_parser_options) {}

  /// Load implicit dependencies for \a edge.
  /// @return false on error (without filling \a err if info is just missing
  //                          or out of date).
  bool LoadDeps(Edge* edge, std::string* err);

  DepsLog* deps_log() const { return deps_log_; }

 protected:
  /// Process loaded implicit dependencies for \a edge and update the graph
  /// @return false on error (without filling \a err if info is just missing)
  virtual bool ProcessDepfileDeps(Edge* edge, std::vector<StringPiece>* depfile_ins, std::string* err);

  /// Load implicit dependencies for \a edge from a depfile attribute.
  /// @return false on error (without filling \a err if info is just missing).
  bool LoadDepFile(Edge* edge, const std::string& path, std::string* err);

  /// Load implicit dependencies for \a edge from the DepsLog.
  /// @return false on error (without filling \a err if info is just missing).
  bool LoadDepsFromLog(Edge* edge, std::string* err);

  /// Preallocate \a count spaces in the input array on \a edge, returning
  /// an iterator pointing at the first new space.
  std::vector<Node*>::iterator PreallocateSpace(Edge* edge, int count);

  State* state_;
  DiskInterface* disk_interface_;
  DepsLog* deps_log_;
  DepfileParserOptions const* depfile_parser_options_;
};

/// DependencyScan manages the process of scanning the files in a graph
/// and updating the dirty/outputs_ready state of all the nodes and edges.
struct DependencyScan {
  DependencyScan(State* state, BuildLog* build_log, DepsLog* deps_log, DiskInterface* disk_interface,
                 DepfileParserOptions const* depfile_parser_options)
      : build_log_(build_log), disk_interface_(disk_interface),
        dep_loader_(state, deps_log, disk_interface, depfile_parser_options), dyndep_loader_(state, disk_interface) {}

  /// Update the |dirty_| state of the given nodes by transitively inspecting
  /// their input edges.
  /// Examine inputs, outputs, and command lines to judge whether an edge
  /// needs to be re-run, and update outputs_ready_ and each outputs' |dirty_|
  /// state accordingly.
  /// Appends any validation nodes found to the nodes parameter.
  /// Returns false on failure.
  bool RecomputeDirty(Node* node, std::vector<Node*>* validation_nodes, std::string* err);

  /// Recompute whether any output of the edge is dirty, if so sets |*dirty|.
  /// Returns false on failure.
  bool RecomputeOutputsDirty(Edge* edge, Node* most_recent_input, bool* dirty, std::string* err);

  BuildLog* build_log() const { return build_log_; }
  void set_build_log(BuildLog* log) { build_log_ = log; }

  DepsLog* deps_log() const { return dep_loader_.deps_log(); }

  /// Load a dyndep file from the given node's path and update the
  /// build graph with the new information.  One overload accepts
  /// a caller-owned 'DyndepFile' object in which to store the
  /// information loaded from the dyndep file.
  bool LoadDyndeps(Node* node, std::string* err) const;
  bool LoadDyndeps(Node* node, DyndepFile* ddf, std::string* err) const;

 private:
  bool RecomputeNodeDirty(Node* node, std::vector<Node*>* stack, std::vector<Node*>* validation_nodes,
                          std::string* err);
  bool VerifyDAG(Node* node, std::vector<Node*>* stack, std::string* err);

  /// Recompute whether a given single output should be marked dirty.
  /// Returns true if so.
  bool RecomputeOutputDirty(const Edge* edge, const Node* most_recent_input, const std::string& command, Node* output);

  BuildLog* build_log_;
  DiskInterface* disk_interface_;
  ImplicitDepLoader dep_loader_;
  DyndepLoader dyndep_loader_;
};

#endif  // NINJA_GRAPH_H_
