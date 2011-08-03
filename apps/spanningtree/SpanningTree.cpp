/** Spanning-tree application -*- C++ -*-
 * @file
 *
 * A simple spanning tree algorithm to demostrate the Galois system.
 *
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2011, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @author Donald Nguyen <ddn@cs.utexas.edu>
 */
#include "Galois/Galois.h"
#include "Galois/Bag.h"
#include "Galois/Statistic.h"
#include "Galois/Graphs/Graph.h"
#include "Galois/Graphs/FileGraph.h"
#include "Lonestar/Banner.h"
#include "Lonestar/CommandLine.h"

#include <utility>
#include <vector>
#include <algorithm>
#include <iostream>

namespace {

const char* name = "Spanning-tree Algorithm";
const char* description = "Compute the spanning tree (not mimimal) of a graph";
const char* url = NULL;
const char* help = "<root id> <graph>";

struct Node {
  bool in_mst;
  Node() : in_mst(false) { }
};

typedef Galois::Graph::FirstGraph<Node,void,false> Graph;
typedef Graph::GraphNode GNode;
typedef std::pair<GNode,GNode> Edge;

Graph graph;

struct Process {
  Galois::InsertBag<Edge>& result;
  Process(Galois::InsertBag<Edge>& _result) : result(_result) { }

  template<typename Context>
  void operator()(GNode src, Context& ctx) {
    for (Graph::neighbor_iterator ii = graph.neighbor_begin(src),
        ei = graph.neighbor_end(src); ii != ei; ++ii) {
      GNode dst = *ii;
      Node& data = graph.getData(dst, Galois::NONE);
      if (data.in_mst)
        continue;
      result.push(Edge(src, dst));
      data.in_mst = true;
      ctx.push(dst);
    }
  }
};

void runSerial(const std::vector<GNode>& initial,
    Galois::InsertBag<Edge>& result) {
  std::vector<GNode> worklist;

  for (std::vector<GNode>::const_iterator ii = initial.begin(), ei = initial.end();
      ii != ei; ++ii) {
    worklist.push_back(*ii);
  }

  while (!worklist.empty()) {
    GNode src = worklist.back();
    worklist.pop_back();
    
    // Expand tree
    for (Graph::neighbor_iterator ii = graph.neighbor_begin(src),
        ei = graph.neighbor_end(src); ii != ei; ++ii) {
      GNode dst = *ii;
      Node& data = graph.getData(dst, Galois::NONE);
      if (data.in_mst)
        continue;
      result.push(Edge(src, dst));
      data.in_mst = true;
      worklist.push_back(dst);
    }
  }
}

void runParallel(const std::vector<GNode>& initial,
    Galois::InsertBag<Edge>& result) {
  Galois::for_each(initial.begin(), initial.end(), Process(result));
}

bool verify(Galois::InsertBag<Edge>& result) {
  if (std::distance(result.begin(), result.end()) != graph.size() - 1)
    return false;

  for (Graph::active_iterator src = graph.active_begin(),
      end = graph.active_end(); src != end; ++src) {
    if (!graph.getData(*src).in_mst)
      return false;
  }

  return true;
}

void readGraph(const char* filename, int root_id, GNode* root) {
  typedef Galois::Graph::LC_FileGraph<int,void> ReaderGraph;
  typedef ReaderGraph::GraphNode ReaderGNode;

  ReaderGraph reader;
  reader.structureFromFile(filename);
  reader.emptyNodeData();

  int num_nodes = 0;
  for (ReaderGraph::active_iterator ii = reader.active_begin(),
      ei = reader.active_end(); ii != ei; ++ii, ++num_nodes) {
    reader.getData(*ii) = num_nodes;
  }

  std::vector<GNode> nodes;
  nodes.resize(num_nodes);
  for (int i = 0; i < num_nodes; ++i) {
    Node node;
    GNode src = graph.createNode(node);
    graph.addNode(src);
    nodes[i] = src;
  }

  for (ReaderGraph::active_iterator ii = reader.active_begin(),
      ei = reader.active_end(); ii != ei; ++ii) {
    ReaderGNode src = *ii;
    int src_id = reader.getData(src);
    for (ReaderGraph::neighbor_iterator jj = reader.neighbor_begin(src),
        ej = reader.neighbor_end(src); jj != ej; ++jj) {
      int dst_id = reader.getData(*jj);
      graph.addEdge(nodes[src_id], nodes[dst_id]);
    }
  }

  if (root_id < 0 || root_id >= num_nodes) {
    assert(0 && "Unknown root id");
    abort();
  }

  *root = nodes[root_id];
}

}

int main(int argc, const char** argv) {
  std::vector<const char*> args = parse_command_line(argc, argv, help);

  if (args.size() != 2) {
    std::cerr 
      << "incorrect number of arguments, use -help for usage information\n";
    return 1;
  }
  printBanner(std::cout, name, description, url);

  GNode root;
  readGraph(args[1], atoi(args[0]), &root);

  std::vector<GNode> initial;
  graph.getData(root).in_mst = true;
  initial.push_back(root);

  Galois::InsertBag<Edge> result;

  Galois::StatTimer T;
  T.start();
  if (numThreads) {
    runParallel(initial, result);
  } else {
    runSerial(initial, result);
  }
  T.stop();

  std::cout << "Edges in spanning tree: " 
    << std::distance(result.begin(), result.end()) << "\n";

  if (!skipVerify && !verify(result)) {
    std::cerr << "If graph was connected, verification failed\n";
    assert(0 && "If graph was connected, verification failed");
    abort();
  }

  return 0;
}

