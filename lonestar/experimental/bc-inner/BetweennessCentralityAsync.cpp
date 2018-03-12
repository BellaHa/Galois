/** Async Betweenness centrality -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
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
 * Asynchrounous betweeness-centrality. 
 *
 * @author Dimitrios Prountzos <dprountz@cs.utexas.edu>
 * @author Loc Hoang <l_hoang@utexas.edu>
 */

//#define NDEBUG // Used in Debug build to prevent things from printing

#include "Lonestar/BoilerPlate.h"
#include "galois/ConditionalReduction.h"

#include "galois/graphs/BufferedGraph.h"
#include "galois/graphs/B_LC_CSR_Graph.h"
#include "galois/Bag.h"

#include "BCNode.h"
#include "BCEdge.h"

#include "galois/runtime/Profile.h"

#include <iomanip>

////////////////////////////////////////////////////////////////////////////////
// Command line parameters
////////////////////////////////////////////////////////////////////////////////

namespace cll = llvm::cl;

static cll::opt<std::string> filename(cll::Positional,
                                      cll::desc("<input graph in Galois bin "
                                                "format>"),
                                      cll::Required);

static cll::opt<unsigned int> startNode("startNode", 
                                        cll::desc("Node to start search from"),
                                        cll::init(0));

static cll::opt<unsigned int> numOfSources("numOfSources", 
                                        cll::desc("Number of sources to compute"
                                                  " BC on"),
                                        cll::init(0));

static cll::opt<bool> generateCert("generateCertificate",
                                   cll::desc("Prints certificate at end of "
                                             "execution"),
                                   cll::init(false));
// TODO better description
//static cll::opt<bool> useNodeBased("useNodeBased",
//                                   cll::desc("Use node based execution"),
//                                   cll::init(true));

using NodeType = BCNode<false, true>;
using Graph = galois::graphs::B_LC_CSR_Graph<NodeType, BCEdge, false, true>;

struct BetweenessCentralityAsync {
  Graph& graph;

  BetweenessCentralityAsync(Graph& _graph) : graph(_graph) { }
  
  using Counter = 
    ConditionalAccumulator<galois::GAccumulator<unsigned long>, BC_COUNT_ACTIONS>;
  Counter spfuCount;
  Counter updateSigmaP1Count;
  Counter updateSigmaP2Count;
  Counter firstUpdateCount;
  Counter correctNodeP1Count;
  Counter correctNodeP2Count;
  Counter noActionCount;
  
  using MaxCounter = 
    ConditionalAccumulator<galois::GReduceMax<unsigned long>, BC_COUNT_ACTIONS>;
  MaxCounter largestNodeDist;
  
  using LeafCounter = 
    ConditionalAccumulator<galois::GAccumulator<unsigned long>, BC_COUNT_LEAVES>;
  LeafCounter leafCount;

  void correctNode(uint32_t dstID, BCEdge& ed) {
    NodeType& dstData = graph.getData(dstID);

    // loop through in edges
    for (auto e : graph.in_edges(dstID)) {
      BCEdge& inEdgeData = graph.getInEdgeData(e);

      uint32_t srcID = graph.getInEdgeDst(e);
      if (srcID == dstID) continue;

      NodeType& srcData = graph.getData(srcID);

      // lock in right order
      if (srcID < dstID) { 
        srcData.lock();
        dstData.lock();
      } else { 
        dstData.lock();
        srcData.lock();
      }

      const unsigned edgeLevel = inEdgeData.level; 

      // Correct Node
      if (srcData.distance >= dstData.distance) { 
        correctNodeP1Count.update(1);
        dstData.unlock();

        if (edgeLevel != infinity) {
          inEdgeData.level = infinity;
          if (edgeLevel == srcData.distance) {
            correctNodeP2Count.update(1);
            srcData.nsuccs--;
          }
        }
        srcData.unlock();
      } else {
        srcData.unlock();
        dstData.unlock();
      }
    }
  }

  template<typename CTXType>
  void spAndFU(uint32_t srcID, uint32_t dstID, BCEdge& ed, CTXType& ctx) {
    spfuCount.update(1);

    NodeType& srcData = graph.getData(srcID);
    NodeType& dstData = graph.getData(dstID);

    // make dst a successor of src, src predecessor of dst
    srcData.nsuccs++;
    const uint64_t srcSigma = srcData.sigma;
    NodeType::predTY& dstPreds = dstData.preds;
    bool dstPredsNotEmpty = !dstPreds.empty();
    dstPreds.clear();
    dstPreds.push_back(srcID);
    dstData.distance = srcData.distance + 1;

    largestNodeDist.update(dstData.distance);

    dstData.nsuccs = 0; // SP
    dstData.sigma = srcSigma; // FU
    ed.val = srcSigma;
    ed.level = srcData.distance;
    srcData.unlock();
    dstData.unlock();

    if (!dstData.isAlreadyIn()) ctx.push(dstID);
    if (dstPredsNotEmpty) { correctNode(dstID, ed); }
  }

  template<typename CTXType>
  void updateSigma(uint32_t srcID, uint32_t dstID, BCEdge& ed, CTXType& ctx) {
    updateSigmaP1Count.update(1);

    NodeType& srcData = graph.getData(srcID);
    NodeType& dstData = graph.getData(dstID);

    const uint64_t srcSigma = srcData.sigma;
    const uint64_t eval = ed.val;
    const uint64_t diff = srcSigma - eval;

    srcData.unlock();
    if (diff > 0) {
      updateSigmaP2Count.update(1);
      ed.val = srcSigma;

      uint64_t old = dstData.sigma;
      dstData.sigma += diff;
      if (old >= dstData.sigma) {
        galois::gDebug("Overflow detected; capping at max uint64_t");
        dstData.sigma = std::numeric_limits<uint64_t>::max();
      }

      int nbsuccs = dstData.nsuccs;
      dstData.unlock();

      if (nbsuccs > 0) {
        if (!dstData.isAlreadyIn()) ctx.push(dstID);
      }
    } else {
      dstData.unlock();
    }
  }

  template<typename CTXType>
  void firstUpdate(uint32_t srcID, uint32_t dstID, BCEdge& ed, CTXType& ctx) {
    firstUpdateCount.update(1);

    NodeType& srcData = graph.getData(srcID);
    srcData.nsuccs++;
    const uint64_t srcSigma = srcData.sigma;

    NodeType& dstData = graph.getData(dstID);
    dstData.preds.push_back(srcID);

    const uint64_t dstSigma = dstData.sigma;

    uint64_t old = dstData.sigma;
    dstData.sigma = dstSigma + srcSigma;
    if (old >= dstData.sigma) {
      galois::gDebug("Overflow detected; capping at max uint64_t");
      dstData.sigma = std::numeric_limits<uint64_t>::max();
    }

    ed.val = srcSigma;
    ed.level = srcData.distance;
    srcData.unlock();
    int nbsuccs = dstData.nsuccs;
    dstData.unlock();
    if (nbsuccs > 0) {
      if (!dstData.isAlreadyIn()) ctx.push(dstID);
    }
  }
  
  //template <typename WLForEach, typename WorkListType>
  template <typename WorkListType>
  void dagConstruction(WorkListType& wl) {
    //galois::runtime::profileVtune(
    //[&] () {
    galois::for_each(
      galois::iterate(wl), 
      [&] (uint32_t srcID, auto& ctx) {
        NodeType& srcData = graph.getData(srcID);
        srcData.markOut();
  
        // loop through all edges
        for (auto e : graph.edges(srcID)) {
          BCEdge& edgeData = graph.getEdgeData(e);
          uint32_t dstID = graph.getEdgeDst(e);
          NodeType& dstData = graph.getData(dstID);
          
          if (srcID == dstID) continue; // ignore self loops
  
          // lock in set order to prevent deadlock (lower id first)
          // TODO run even in serial version; find way to not need to run
          if (srcID < dstID) {
            srcData.lock();
            dstData.lock();
          } else {
            dstData.lock();
            srcData.lock();
          }
  
          const int elevel = edgeData.level;
          const int ADist = srcData.distance;
          const int BDist = dstData.distance;
  
          if (BDist - ADist > 1) {
            // Shortest Path + First Update (and Correct Node)
            this->spAndFU(srcID, dstID, edgeData, ctx);
          } else if (elevel == ADist && BDist == ADist + 1) {
            // Update Sigma
            this->updateSigma(srcID, dstID, edgeData, ctx);
          } else if (BDist == ADist + 1 && elevel != ADist) {
            // First Update not combined with Shortest Path
            this->firstUpdate(srcID, dstID, edgeData, ctx);
          } else { // No Action
            noActionCount.update(1);
            srcData.unlock();
            dstData.unlock();
          }
        }
      },
      galois::loopname("ForwardPhase")
      //,galois::wl<WLForEach>()
    );
    //}, "dummy");
  }
  
  template <typename WorkListType>
  void dependencyBackProp(WorkListType& wl) {
    galois::for_each(
      galois::iterate(wl),
      [&] (uint32_t srcID, auto& ctx) {
        NodeType& srcData = graph.getData(srcID);
        srcData.lock();
  
        if (srcData.nsuccs == 0) {
          const double srcDelta = srcData.delta;
          srcData.bc += srcDelta;
  
          srcData.unlock();
  
          NodeType::predTY& srcPreds = srcData.preds;
  
          // loop through src's predecessors
          for (unsigned i = 0; i < srcPreds.size(); i++) {
            uint32_t predID = srcPreds[i];
            NodeType& predData = graph.getData(predID);

            const double term = (double)predData.sigma * (1.0 + srcDelta) / 
                                srcData.sigma; 
            predData.lock();
            predData.delta += term;
            const unsigned prevPdNsuccs = predData.nsuccs;
            predData.nsuccs--;
  
            if (prevPdNsuccs == 1) {
              predData.unlock();
              ctx.push(predID);
            } else {
              predData.unlock();
            }
          }
          srcData.reset();

          // reset edge data
          for (auto e : graph.edges(srcID)) {
            graph.getEdgeData(e).reset();
          }
        } else {
          srcData.unlock();
        }
      },
      galois::loopname("BackwardPhase")
    );
  }
  
  galois::InsertBag<uint32_t>* fringewl;
  
  void findLeaves(unsigned nnodes) {
    galois::do_all(
      galois::iterate(0u, nnodes),
      [&] (auto i) {
        NodeType& n = graph.getData(i);

        if (n.nsuccs == 0 && n.distance < infinity) {
          leafCount.update(1);
          fringewl->push(i);
        }
      },
      galois::loopname("LeafFind")
    );
  }
};

//struct NodeIndexer : std::binary_function<NodeType*, int, int> {
//  int operator() (const NodeType *val) const {
//    return val->distance;
//  }
//};

static const char* name = "Betweenness Centrality";
static const char* desc = "Computes betwenness centrality in an unweighted "
                          "graph";

int main(int argc, char** argv) {
  galois::SharedMemSys G;
  LonestarStart(argc, argv, name, desc, NULL);

  if (BC_CONCURRENT) {
    galois::gInfo("Running in concurrent mode with ", numThreads, " threads");
  } else {
    galois::gInfo("Running in serial mode");
  }

  Graph bcGraph;
  galois::graphs::BufferedGraph<void> fileReader;
  fileReader.loadGraph(filename);
  bcGraph.allocateFrom(fileReader.size(), fileReader.sizeEdges());

  galois::do_all(
    galois::iterate((uint32_t)0, fileReader.size()),
    [&] (uint32_t i) {
      auto b = fileReader.edgeBegin(i);
      auto e = fileReader.edgeEnd(i);

      bcGraph.fixEndEdge(i, *e);

      while (b < e) {
        bcGraph.constructEdge(*b, fileReader.edgeDestination(*b));
        b++;
      }
    }
  );
  bcGraph.constructIncomingEdges();

  //for (auto k : bcGraph.in_edges(0)) {
  //  BCEdge& asdf = bcGraph.getInEdgeData(k);
  //  asdf.reset();
  //  printf("%s\n", asdf.toString().c_str());
  //}

  //exit(0);

  //BCGraph graph(filename.c_str());

  BetweenessCentralityAsync bcExecutor(bcGraph);

  unsigned nnodes = bcGraph.size();
  uint64_t nedges = bcGraph.sizeEdges();
  galois::gInfo("Num nodes is ", nnodes, ", num edges is ", nedges);

  bcExecutor.spfuCount.reset();
  bcExecutor.updateSigmaP1Count.reset();
  bcExecutor.updateSigmaP2Count.reset();
  bcExecutor.firstUpdateCount.reset();
  bcExecutor.correctNodeP1Count.reset();
  bcExecutor.correctNodeP2Count.reset();
  bcExecutor.noActionCount.reset();
  bcExecutor.largestNodeDist.reset();

  const int chunksize = 8;
  galois::gInfo("Using chunk size : ", chunksize);
  //typedef galois::worklists::OrderedByIntegerMetric<NodeIndexer, 
  //                         galois::worklists::dChunkedLIFO<chunksize> > wl2ty;
  galois::InsertBag<uint32_t> wl2;
  galois::InsertBag<uint32_t> wl4;
  bcExecutor.fringewl = &wl4;

  galois::reportPageAlloc("MemAllocPre");
  galois::preAlloc(galois::getActiveThreads() * nnodes / 2000000);
  galois::reportPageAlloc("MemAllocMid");

  galois::StatTimer executionTimer;

  // reset everything in preparation for run
  // TODO refactor
  galois::do_all(
    galois::iterate(0u, nnodes),
    [&] (auto i) {
      bcGraph.getData(i).reset();
    }
  );

  galois::do_all(
    galois::iterate(0ul, nedges),
    [&] (auto i) {
      bcGraph.getEdgeData(i).reset();
    }
  );

  if (numOfSources == 0) {
    numOfSources = nnodes;
  }

  executionTimer.start();
  for (uint32_t i = startNode; i < numOfSources; ++i) {
    // ignore nodes with no neighbors
    if (!std::distance(bcGraph.edge_begin(i), bcGraph.edge_end(i))) {
      continue;
    }

    std::vector<uint32_t> wl;
    wl2.push_back(i);
    NodeType& active = bcGraph.getData(i);
    active.initAsSource();
    //galois::gDebug("Source is ", i);

    //bcExecutor.dagConstruction<wl2ty>(wl2);
    bcExecutor.dagConstruction(wl2);
    wl2.clear();

    //if (DOCHECKS) graph.checkGraph(active);

    bcExecutor.leafCount.reset();
    bcExecutor.findLeaves(nnodes);

    if (bcExecutor.leafCount.isActive()) {
      galois::gPrint(bcExecutor.leafCount.reduce(), " leaf nodes in DAG\n");
    }

    double backupSrcBC = active.bc;
    bcExecutor.dependencyBackProp(wl4);

    active.bc = backupSrcBC; // current source BC should not get updated

    wl4.clear();
    //if (DOCHECKS) graph.checkSteadyState2();

    // TODO refactor
    galois::do_all(
      galois::iterate(0u, nnodes),
      [&] (auto i) {
        bcGraph.getData(i).reset();
      },
      galois::loopname("CleanupLoop")
    );

    galois::do_all(
      galois::iterate(0ul, nedges),
      [&] (auto i) {
        bcGraph.getEdgeData(i).reset();
      },
      galois::loopname("CleanupLoop")
    );
  }
  executionTimer.stop();

  galois::reportPageAlloc("MemAllocPost");

  // one counter active -> all of them are active (since all controlled by same
  // ifdef)
  if (bcExecutor.spfuCount.isActive()) {
    galois::gPrint("SP&FU ", bcExecutor.spfuCount.reduce(), 
                   "\nUpdateSigmaBefore ", bcExecutor.updateSigmaP1Count.reduce(), 
                   "\nRealUS ", bcExecutor.updateSigmaP2Count.reduce(), 
                   "\nFirst Update ", bcExecutor.firstUpdateCount.reduce(), 
                   "\nCorrectNodeBefore ", bcExecutor.correctNodeP1Count.reduce(), 
                   "\nReal CN ", bcExecutor.correctNodeP2Count.reduce(), 
                   "\nNoAction ", bcExecutor.noActionCount.reduce(), "\n");
    galois::gPrint("Largest node distance is ", bcExecutor.largestNodeDist.reduce(), "\n");
  }

  // prints out first 10 node BC values
  if (!skipVerify) {
    //graph.verify(); // TODO see what this does
    int count = 0;
    for (unsigned i = 0; i < nnodes && count < 10; ++i, ++count) {
      galois::gPrint(count, ": ", std::setiosflags(std::ios::fixed), 
                     std::setprecision(6), bcGraph.getData(i).bc, "\n");
    }
  }

  if (generateCert) {
    std::cerr << "Writting out bc values...\n";
    std::stringstream outfname;
    outfname << "certificate" << "_" << numThreads << ".txt";
    std::string fname = outfname.str();
    std::ofstream outfile(fname.c_str());
    for (unsigned i=0; i<nnodes; ++i) {
      outfile << i << " " << std::setiosflags(std::ios::fixed) 
              << std::setprecision(9) << bcGraph.getData(i).bc << "\n";
    }
    outfile.close();
  }

  return 0;
}
