/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
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
 */

#include <arrow/type.h>

#include "katana/TypedPropertyGraph.h"
#include "katana/analytics/Utils.h"
#include "pagerank-impl.h"

namespace {

struct PagerankValueAndOutDegreeTy {
  uint32_t out;
  PRTy value;
};

using PagerankValueAndOutDegree =
    katana::StructProperty<PagerankValueAndOutDegreeTy>;

using NodeData = std::tuple<PagerankValueAndOutDegree>;
using EdgeData = std::tuple<>;

typedef katana::TypedPropertyGraph<NodeData, EdgeData> Graph;
typedef typename Graph::Node GNode;

using DeltaArray = katana::LargeArray<PRTy>;
using ResidualArray = katana::LargeArray<PRTy>;

//! Initialize nodes for the topological algorithm.
void
InitNodeDataTopological(Graph* graph) {
  PRTy init_value = 1.0f / graph->size();
  katana::do_all(
      katana::iterate(*graph),
      [&](const GNode& n) {
        auto& sdata = graph->GetData<PagerankValueAndOutDegree>(n);
        sdata.value = init_value;
        sdata.out = 0;
      },
      katana::loopname("initNodeData"));
}

//! Initialize nodes for the residual algorithm.
void
InitNodeDataResidual(
    Graph* graph, DeltaArray& delta, ResidualArray& residual,
    katana::analytics::PagerankPlan plan) {
  katana::do_all(
      katana::iterate(*graph),
      [&](const GNode& n) {
        auto& sdata = graph->GetData<PagerankValueAndOutDegree>(n);
        sdata.value = 0;
        sdata.out = 0;
        delta[n] = 0;
        residual[n] = plan.initial_residual();
      },
      katana::loopname("initNodeData"));
}

//! Computing outdegrees in the tranpose graph is equivalent to computing the
//! indegrees in the original graph.
void
ComputeOutDeg(Graph* graph) {
  katana::StatTimer out_degree_timer("computeOutDegFunc");
  out_degree_timer.start();

  katana::LargeArray<std::atomic<size_t>> vec;
  vec.allocateInterleaved(graph->size());

  katana::do_all(
      katana::iterate(*graph),
      [&](const GNode& src) { vec.constructAt(src, 0ul); },
      katana::loopname("InitDegVec"));

  katana::do_all(
      katana::iterate(*graph),
      [&](const GNode& src) {
        for (auto nbr : graph->edges(src)) {
          auto dest = graph->GetEdgeDest(nbr);
          vec[*dest].fetch_add(1ul);
        }
      },
      katana::steal(),
      katana::chunk_size<katana::analytics::PagerankPlan::kChunkSize>(),
      katana::loopname("ComputeOutDeg"));

  katana::do_all(
      katana::iterate(*graph),
      [&](const GNode& src) {
        auto& sdata = graph->GetData<PagerankValueAndOutDegree>(src);
        sdata.out = vec[src];
      },
      katana::loopname("CopyDeg"));

  out_degree_timer.stop();
}

/**
 * It does not calculate the pagerank for each iteration,
 * but only calculate the residual to be added from the previous pagerank to
 * the current one.
 * If the residual is smaller than the tolerance, that is not reflected to
 * the next pagerank.
 */
//! [scalarreduction]
void
ComputePRResidual(
    Graph* graph, DeltaArray& delta, ResidualArray& residual,
    katana::analytics::PagerankPlan plan) {
  unsigned int iterations = 0;
  katana::GAccumulator<unsigned int> accum;

  while (true) {
    katana::do_all(
        katana::iterate(*graph),
        [&](const GNode& src) {
          auto& sdata = graph->GetData<PagerankValueAndOutDegree>(src);
          delta[src] = 0;

          //! Only the residual higher than tolerance will be reflected
          //! to the pagerank.
          if (residual[src] > plan.tolerance()) {
            PRTy old_residual = residual[src];
            residual[src] = 0.0;
            sdata.value += old_residual;
            if (sdata.out > 0) {
              delta[src] = old_residual * plan.alpha() / sdata.out;
              accum += 1;
            }
          }
        },
        katana::loopname("PageRank_delta"));

    katana::do_all(
        katana::iterate(*graph),
        [&](const GNode& src) {
          float sum = 0;
          for (auto nbr : graph->edges(src)) {
            auto dest = graph->GetEdgeDest(nbr);
            if (delta[*dest] > 0) {
              sum += delta[*dest];
            }
          }
          if (sum > 0) {
            residual[src] = sum;
          }
        },
        katana::steal(),
        katana::chunk_size<katana::analytics::PagerankPlan::kChunkSize>(),
        katana::loopname("PageRank"));

#if DEBUG
    std::cout << "iteration: " << iterations << "\n";
#endif
    iterations++;
    if (iterations >= plan.max_iterations() || !accum.reduce()) {
      break;
    }
    accum.reset();
  }  ///< End while(true).
  //! [scalarreduction]
}

/**
 * PageRank pull topological.
 * Always calculate the new pagerank for each iteration.
 */
void
ComputePRTopological(Graph* graph, katana::analytics::PagerankPlan plan) {
  unsigned int iteration = 0;
  katana::GAccumulator<float> accum;

  float base_score = (1.0f - plan.alpha()) / graph->size();
  while (true) {
    katana::do_all(
        katana::iterate(*graph),
        [&](const GNode& src) {
          auto& sdata = graph->GetData<PagerankValueAndOutDegree>(src);
          float sum = 0.0;

          for (auto jj : graph->edges(src)) {
            auto dest = graph->GetEdgeDest(jj);
            auto& ddata = graph->GetData<PagerankValueAndOutDegree>(dest);
            sum += ddata.value / ddata.out;
          }

          //! New value of pagerank after computing contributions from
          //! incoming edges in the original graph.
          float value = sum * plan.alpha() + base_score;
          //! Find the delta in new and old pagerank values.
          float diff = std::fabs(value - sdata.value);

          //! Do not update pagerank before the diff is computed since
          //! there is a data dependence on the pagerank value.
          sdata.value = value;
          accum += diff;
        },
        katana::steal(),
        katana::chunk_size<katana::analytics::PagerankPlan::kChunkSize>(),
        katana::loopname("Pagerank Topological"));

#if DEBUG
    std::cout << "iteration: " << iteration << " max delta: " << delta << "\n";
#endif
    iteration += 1;
    if (accum.reduce() <= plan.tolerance() ||
        iteration >= plan.max_iterations()) {
      break;
    }
    accum.reset();

  }  ///< End while(true).

  katana::ReportStatSingle("PageRank", "Iterations", iteration);
}

katana::Result<void>
ExtractValueFromTopoGraph(
    katana::PropertyGraph* pg, const Graph& from,
    const std::string& output_property_name) {
  if (auto result =
          katana::analytics::ConstructNodeProperties<std::tuple<NodeValue>>(
              pg, {output_property_name});
      !result) {
    return result.error();
  }

  auto graph_result =
      katana::TypedPropertyGraph<std::tuple<NodeValue>, std::tuple<>>::Make(
          pg, {output_property_name}, {});
  if (!graph_result) {
    return graph_result.error();
  }
  auto graph = graph_result.value();

  katana::do_all(
      katana::iterate(from),
      [&](uint32_t i) {
        PRTy rank = from.GetData<PagerankValueAndOutDegree>(i).value;
        graph.GetData<NodeValue>(i) = rank;
      },
      katana::loopname("Extract pagerank"), katana::no_stats());

  return katana::ResultSuccess();
}

}  // namespace

katana::Result<void>
PagerankPullTopological(
    katana::PropertyGraph* pg, const std::string& output_property_name,
    katana::analytics::PagerankPlan plan) {
  katana::EnsurePreallocated(2, 3 * pg->num_nodes() * sizeof(NodeData));
  katana::analytics::TemporaryPropertyGuard temporary_property{pg};
  if (auto result = katana::analytics::ConstructNodeProperties<NodeData>(
          pg, {temporary_property.name()});
      !result) {
    return result.error();
  }

  auto compute_graph_result = Graph::Make(pg, {temporary_property.name()}, {});
  if (!compute_graph_result) {
    return compute_graph_result.error();
  }
  Graph graph = compute_graph_result.value();

  InitNodeDataTopological(&graph);
  ComputeOutDeg(&graph);

  katana::StatTimer exec_time("PagerankPullTopological");
  exec_time.start();
  ComputePRTopological(&graph, plan);
  exec_time.stop();

  return ExtractValueFromTopoGraph(pg, graph, output_property_name);
}

katana::Result<void>
PagerankPullResidual(
    katana::PropertyGraph* pg, const std::string& output_property_name,
    katana::analytics::PagerankPlan plan) {
  katana::EnsurePreallocated(2, 3 * pg->num_nodes() * sizeof(NodeData));

  if (auto result = katana::analytics::ConstructNodeProperties<NodeData>(
          pg, {output_property_name});
      !result) {
    return result.error();
  }

  auto graph_result = Graph::Make(pg, {output_property_name}, {});
  if (!graph_result) {
    return graph_result.error();
  }
  Graph graph = graph_result.value();

  DeltaArray delta;
  delta.allocateInterleaved(pg->num_nodes());
  ResidualArray residual;
  residual.allocateInterleaved(pg->num_nodes());

  InitNodeDataResidual(&graph, delta, residual, plan);
  ComputeOutDeg(&graph);

  katana::StatTimer exec_time("PagerankPullResidual");
  exec_time.start();
  ComputePRResidual(&graph, delta, residual, plan);
  exec_time.stop();

  return katana::ResultSuccess();
}
