#include <ATen/core/jit_type.h>
#ifdef USE_VULKAN
#include <ATen/native/vulkan/VulkanOpContext.h>
#endif

#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/ir/subgraph_matcher.h>
#include <torch/csrc/jit/passes/constant_pooling.h>
#include <torch/csrc/jit/passes/fold_conv_bn.h>
#include <torch/csrc/jit/passes/freeze_module.h>
#include <torch/csrc/jit/passes/fuse_linear.h>
#include <torch/csrc/jit/passes/graph_rewrite_helper.h>
#include <torch/csrc/jit/passes/prepack_folding.h>
#include <torch/csrc/jit/passes/remove_dropout.h>
#include <torch/csrc/jit/passes/remove_mutation.h>
#include <torch/csrc/jit/passes/subgraph_rewrite.h>
#include <torch/csrc/jit/passes/vulkan_rewrite.h>
#include <torch/csrc/jit/runtime/graph_executor_impl.h>

namespace torch {
namespace jit {

#ifdef USE_VULKAN

namespace {

void insertPrePackedLinearOp(std::shared_ptr<Graph>& graph) {
  // fuse decomposed linear into aten::linear
  FuseLinear(graph);

  const std::string linear_before_inline = R"(
    graph(%linear, %input, %weight, %bias):
        %res = prim::CallFunction(%linear, %input, %weight, %bias)
        return (%res))";

  const std::string prepacked_ops_pattern_before_inline = R"(
    graph(%linear, %input, %weight, %bias):
        %weight_t = aten::t(%weight)
        %packed_weight_bias = vulkan_prepack::linear_prepack(
            %weight_t, %bias)
        %res = vulkan_prepack::linear_run(%input, %packed_weight_bias)
        return (%res))";

  const auto filter = [](const Match& match,
                         const std::unordered_map<std::string, Value*>& vmap) {
    const auto& match_vmap = match.values_map;
    const auto linear_value = match_vmap.at(vmap.at("linear"));
    const auto func_name = graph_rewrite_helper::getFuncName(linear_value);
    return (func_name == "linear");
  };

  const std::vector<std::pair<std::string, std::string>> value_mappings(
      {{"weight_t", "res"}, {"packed_weight_bias", "res"}, {"res", "res"}});

  SubgraphRewriter linear_call_fn_rewriter;
  linear_call_fn_rewriter.RegisterRewritePattern(
      linear_before_inline,
      prepacked_ops_pattern_before_inline,
      value_mappings);
  linear_call_fn_rewriter.runOnGraph(graph, filter);

  const std::string linear_pattern = R"(
    graph(%input, %weight, %bias):
        %res = aten::linear(%input, %weight, %bias)
        return (%res))";
  const std::string prepacked_ops_pattern = R"(
    graph(%input, %weight, %bias):
        %weight_t = aten::t(%weight)
        %packed_weight_bias = vulkan_prepack::linear_prepack(
            %weight_t, %bias)
        %res = vulkan_prepack::linear_run(%input, %packed_weight_bias)
        return (%res))";

  SubgraphRewriter linear_rewriter;
  linear_rewriter.RegisterRewritePattern(
      linear_pattern, prepacked_ops_pattern, value_mappings);
  linear_rewriter.runOnGraph(graph);
}

void insertPrePackedConv2dOp(std::shared_ptr<Graph>& graph) {
  graph_rewrite_helper::replaceConvolutionWithAtenConv(graph);

  const std::string conv_2d_pattern = R"(
    graph(%input, %weight, %bias, %stride:int[], %padding:int[], %dilation:int[], %groups:int):
        %res = aten::conv2d(%input, %weight, %bias, %stride, %padding, %dilation, %groups)
        return (%res) )";

  const std::string prepacked_ops_conv2d_pattern = R"(
    graph(%input, %weight, %bias, %stride:int[], %padding:int[], %dilation:int[], %groups:int):
        %output_min_max : None = prim::Constant()
        %packed_weight_bias = vulkan_prepack::conv2d_clamp_prepack(
            %weight, %bias, %stride, %padding, %dilation, %groups,
            %output_min_max, %output_min_max)
        %res = vulkan_prepack::conv2d_clamp_run(%input, %packed_weight_bias)
        return (%res) )";

  const std::vector<std::pair<std::string, std::string>> value_mappings(
      {{"output_min_max", "res"},
       {"packed_weight_bias", "res"},
       {"res", "res"}});

  SubgraphRewriter rewriter;
  rewriter.RegisterRewritePattern(
      conv_2d_pattern, prepacked_ops_conv2d_pattern, value_mappings);
  rewriter.runOnGraph(graph);

  const std::string conv_2d_transpose_pattern = R"(
      graph(%input, %weight, %bias, %stride:int[], %padding:int[], %dilation:int[],
          %output_padding:int[], %groups:int):
        %res = aten::conv_transpose2d(%input, %weight, %bias, %stride, %padding, %output_padding, %groups, %dilation)
        return (%res) )";

  const std::string prepacked_ops_conv2d_transpose_pattern = R"(
    graph(%input, %weight, %bias, %stride:int[], %padding:int[], %dilation:int[], %output_padding:int[], %groups:int):
        %output_min_max : None = prim::Constant()
        %packed_weight_bias = vulkan_prepack::conv2d_transpose_clamp_prepack(
            %weight, %bias, %stride, %padding, %output_padding, %dilation, %groups,
            %output_min_max, %output_min_max)
        %res = vulkan_prepack::conv2d_transpose_clamp_run(%input, %packed_weight_bias)
        return (%res) )";

  SubgraphRewriter transpose_rewriter;
  transpose_rewriter.RegisterRewritePattern(
      conv_2d_transpose_pattern,
      prepacked_ops_conv2d_transpose_pattern,
      value_mappings);
  transpose_rewriter.runOnGraph(graph);
}

void fuseHardtanhWithPackedOps(std::shared_ptr<Graph>& graph) {
  const std::string conv2d_prepack_run_hardtanh = R"(
    graph(%input, %weight, %bias, %stride:int[], %padding:int[],
          %dilation:int[], %groups:int, %output_min, %output_max, %dummy_min_max):
        %packed_weight_bias = vulkan_prepack::conv2d_clamp_prepack(
            %weight, %bias, %stride, %padding, %dilation, %groups,
            %dummy_min_max, %dummy_min_max)
        %conv2d_res = vulkan_prepack::conv2d_clamp_run(%input, %packed_weight_bias)
        %res = aten::hardtanh(%conv2d_res, %output_min, %output_max)
        return (%res) )";

  const std::string conv2d_prepack_run_hardtanh_inplace = R"(
    graph(%input, %weight, %bias, %stride:int[], %padding:int[],
          %dilation:int[], %groups:int, %output_min, %output_max, %dummy_min_max):
        %packed_weight_bias = vulkan_prepack::conv2d_clamp_prepack(
            %weight, %bias, %stride, %padding, %dilation, %groups,
            %dummy_min_max, %dummy_min_max)
        %conv2d_res = vulkan_prepack::conv2d_clamp_run(%input, %packed_weight_bias)
        %res = aten::hardtanh_(%conv2d_res, %output_min, %output_max)
        return (%res) )";

  const std::string conv2d_prepack_run_hardtanh_fused = R"(
    graph(%input, %weight, %bias, %stride:int[], %padding:int[],
          %dilation:int[], %groups:int, %output_min, %output_max, %dummy_min_max):
        %packed_weight_bias : __torch__.torch.classes.vulkan.Conv2dOpContext = vulkan_prepack::conv2d_clamp_prepack(
            %weight, %bias, %stride, %padding, %dilation, %groups,
            %output_min, %output_max)
        %res = vulkan_prepack::conv2d_clamp_run(%input, %packed_weight_bias)
        return (%res) )";

  const std::vector<std::pair<std::string, std::string>> value_mappings(
      {{"packed_weight_bias", "packed_weight_bias"}, {"res", "res"}});

  SubgraphRewriter rewriter;
  rewriter.RegisterRewritePattern(
      conv2d_prepack_run_hardtanh,
      conv2d_prepack_run_hardtanh_fused,
      value_mappings);
  rewriter.RegisterRewritePattern(
      conv2d_prepack_run_hardtanh_inplace,
      conv2d_prepack_run_hardtanh_fused,
      value_mappings);
  rewriter.runOnGraph(graph, torch::jit::graph_rewrite_helper::isClampFusable);
}

void fuseReluWithPackedOps(std::shared_ptr<Graph>& graph) {
  const std::string conv2d_prepack_run_relu = R"(
    graph(%input, %weight, %bias, %stride:int[], %padding:int[],
          %dilation:int[], %groups:int, %dummy_min_max):
        %packed_weight_bias = vulkan_prepack::conv2d_clamp_prepack(
            %weight, %bias, %stride, %padding, %dilation, %groups,
            %dummy_min_max, %dummy_min_max)
        %conv2d_res = vulkan_prepack::conv2d_clamp_run(%input, %packed_weight_bias)
        %res = aten::relu(%conv2d_res)
        return (%res) )";

  const std::string conv2d_prepack_run_relu_inplace = R"(
    graph(%input, %weight, %bias, %stride:int[], %padding:int[],
          %dilation:int[], %groups:int, %dummy_min_max):
        %packed_weight_bias = vulkan_prepack::conv2d_clamp_prepack(
            %weight, %bias, %stride, %padding, %dilation, %groups,
            %dummy_min_max, %dummy_min_max)
        %conv2d_res = vulkan_prepack::conv2d_clamp_run(%input, %packed_weight_bias)
        %res = aten::relu_(%conv2d_res)
        return (%res) )";

  const std::string conv2d_prepack_run_relu_fused = R"(
    graph(%input, %weight, %bias, %stride:int[], %padding:int[],
          %dilation:int[], %groups:int, %dummy_min_max):
        %output_min: float = prim::Constant[value=0.0]()
        %output_max: None = prim::Constant()
        %packed_weight_bias : __torch__.torch.classes.vulkan.Conv2dOpContext = vulkan_prepack::conv2d_clamp_prepack(
            %weight, %bias, %stride, %padding, %dilation, %groups,
            %output_min, %output_max)
        %res = vulkan_prepack::conv2d_clamp_run(%input, %packed_weight_bias)
        return (%res) )";

  const std::vector<std::pair<std::string, std::string>> value_mappings(
      {{"output_min", "packed_weight_bias"},
       {"output_max", "packed_weight_bias"},
       {"packed_weight_bias", "packed_weight_bias"},
       {"res", "res"}});

  SubgraphRewriter rewriter;
  rewriter.RegisterRewritePattern(
      conv2d_prepack_run_relu, conv2d_prepack_run_relu_fused, value_mappings);
  rewriter.RegisterRewritePattern(
      conv2d_prepack_run_relu_inplace, conv2d_prepack_run_relu_fused, value_mappings);
  rewriter.runOnGraph(graph, torch::jit::graph_rewrite_helper::isClampFusable);
}

} // namespace

void vulkanInsertPrePackedOps(std::shared_ptr<Graph>& graph) {
  insertPrePackedLinearOp(graph);
  insertPrePackedConv2dOp(graph);
}

void vulkanInsertPrePackedOps(script::Module& module) {
  for (auto& method : module.get_methods()) {
    auto graph = method.graph();
    vulkanInsertPrePackedOps(graph);
  }
  for (script::Module m : module.children()) {
    vulkanInsertPrePackedOps(m);
  }
}

void vulkanFusePrePackedConvWithClamp(script::Module& module) {
  auto graph = module.get_method("forward").graph();
  fuseReluWithPackedOps(graph);
  fuseHardtanhWithPackedOps(graph);
}

void vulkanFoldPrePackingOps(script::Module& m) {
  PrePackingOpsFilterFn filter_fn = [](const Node* n) -> bool {
    return (
        (n->kind() ==
         Symbol::fromQualString("vulkan_prepack::conv2d_clamp_prepack")) ||
        (n->kind() ==
         Symbol::fromQualString("vulkan_prepack::linear_prepack")) ||
        (n->kind() ==
         Symbol::fromQualString(
             "vulkan_prepack::conv2d_transpose_clamp_prepack")));
  };
  PrePackingOpsFolder(m, filter_fn, "prepack_folding");
}

void vulkanRemoveMutation(script::Module& module) {
  auto graph = module.get_method("forward").graph();
  RemoveTensorMutation(graph);
}

void vulkanRunCanonicalOptimizations(script::Module& module) {
  auto graph = module.get_method("forward").graph();
  for (const auto& method : module.get_methods()) {
    auto graph = method.graph();
    runOptimization(graph, false /* no loop unrolling */);
  }
}

script::Module vulkanOptimizeForMobile(
    const script::Module& m,
    const std::vector<std::string>& preserved_methods) {
  auto cloned_module = m.clone();
  cloned_module.eval();
  cloned_module = FoldConvBatchNorm(cloned_module);
  vulkanInsertPrePackedOps(cloned_module);
  cloned_module = freeze_module(cloned_module, preserved_methods);
  vulkanFusePrePackedConvWithClamp(cloned_module);
  vulkanFoldPrePackingOps(cloned_module);
  removeDropout(cloned_module);
  vulkanRemoveMutation(cloned_module);
  // remove duplicated constants
  vulkanRunCanonicalOptimizations(cloned_module);

  cloned_module.register_attribute(
      "optimized_for_vulkan", BoolType::get(), true);
  return cloned_module;
}

#else

void vulkanInsertPrePackedOps(std::shared_ptr<Graph>& graph) {
  TORCH_INTERNAL_ASSERT(
      false, "Vulkan is not enabled. Please build with USE_VULKAN=1");
}

void vulkanInsertPrePackedOps(script::Module& module) {
  TORCH_INTERNAL_ASSERT(
      false, "Vulkan is not enabled. Please build with USE_VULKAN=1");
}

void vulkanFusePrePackedConvWithClamp(script::Module& module) {
  TORCH_INTERNAL_ASSERT(
      false, "Vulkan is not enabled. Please build with USE_VULKAN=1");
}

void vulkanFoldPrePackingOps(script::Module& m) {
  TORCH_INTERNAL_ASSERT(
      false, "Vulkan is not enabled. Please build with USE_VULKAN=1");
}

script::Module vulkanOptimizeForMobile(
    const script::Module& module,
    const std::vector<std::string>& preserved_methods) {
  TORCH_INTERNAL_ASSERT(
      false,
      "Mobile optimizaiton only available with Vulkan at the moment. "
      "Vulkan is not enabled. Please build with USE_VULKAN=1");
  return module;
}

#endif
} // namespace jit
} // namespace torch
