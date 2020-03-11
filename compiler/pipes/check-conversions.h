#pragma once

#include "compiler/function-pass.h"

/*
 * Пайп, который после inferring'а запрещает странные конверсии (op_conv*).
 */
class CheckConversionsPass : public FunctionPassBase {
  static const std::multimap<Operation, PrimitiveType> forbidden_conversions;
public:
  std::string get_description() override {
    return "CheckConversions";
  }

  VertexPtr on_enter_vertex(VertexPtr vertex, LocalT *local __attribute__((unused)));
};
