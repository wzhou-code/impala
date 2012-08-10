// Copyright (c) 2011 Cloudera, Inc. All rights reserved.
#include <boost/algorithm/string.hpp>
#include <glog/logging.h>

#include "codegen/llvm-codegen.h"
#include "runtime/descriptors.h"
#include "runtime/mem-pool.h"
#include "runtime/runtime-state.h"
#include "runtime/string-value.h"
#include "runtime/timestamp-value.h"
#include "runtime/tuple.h"
#include "text-converter.h"
#include "util/string-parser.h"

using namespace boost;
using namespace impala;
using namespace llvm;
using namespace std;

TextConverter::TextConverter(char escape_char) 
  : escape_char_(escape_char) {
}

void TextConverter::UnescapeString(StringValue* value, MemPool* pool) {
  char* new_data = reinterpret_cast<char*>(pool->Allocate(value->len));
  UnescapeString(value->ptr, new_data, &value->len);
  value->ptr = new_data;
}

void TextConverter::UnescapeString(const char* src, char* dest, int* len) {
  char* dest_ptr = dest;
  const char* end = src + *len;
  bool escape_next_char = false;
  while (src < end) {
    if (*src == escape_char_) {
      escape_next_char = !escape_next_char;
    } else {
      escape_next_char = false;
    }
    if (escape_next_char) {
      ++src;
    } else {
      *dest_ptr++ = *src++;
    }
  }
  char* dest_start = reinterpret_cast<char*>(dest);
  *len = dest_ptr - dest_start;
}

// Codegen for a function to parse one slot.  The IR for a int slot looks like:
// define i1 @WriteSlot({ i8, i32 }* %tuple_arg, i8* %data, i32 %len) {
// entry:
//   %parse_result = alloca i32
//   %0 = icmp eq i32 %len, 0
//   br i1 %0, label %set_null, label %parse_slot
// 
// set_null:                                         ; preds = %entry
//   call void @SetNull({ i8, i32 }* %tuple_arg)
//   ret i1 true
// 
// parse_slot:                                       ; preds = %entry
//   %slot = getelementptr inbounds { i8, i32 }* %tuple_arg, i32 0, i32 1
//   %1 = call i32 @IrStringToInt32(i8* %data, i32 %len, i32* %parse_result)
//   %parse_result1 = load i32* %parse_result
//   %failed = icmp eq i32 %parse_result1, 1
//   br i1 %failed, label %parse_fail, label %parse_success
// 
// parse_success:                                    ; preds = %parse_slot
//   store i32 %1, i32* %slot
//   ret i1 true
// 
// parse_fail:                                       ; preds = %parse_slot
//   call void @SetNull({ i8, i32 }* %tuple_arg)
//   ret i1 false
// }
Function* TextConverter::CodegenWriteSlot(LlvmCodeGen* codegen, 
    TupleDescriptor* tuple_desc, SlotDescriptor* slot_desc) {
  SCOPED_TIMER(codegen->codegen_timer());

  // Don't codegen with escape characters.  
  // TODO: handle this case and also the case to copy strings for data compaction
  if (slot_desc->type() == TYPE_STRING && escape_char_ != '\0') {
    LOG(WARNING) << "Could not codegen WriteSlot because escape characters "
                 << "are not yet supported.";
    return NULL;
  }

  StructType* tuple_type = tuple_desc->GenerateLlvmStruct(codegen);
  if (tuple_type == NULL) return NULL;
  PointerType* tuple_ptr_type = PointerType::get(tuple_type, 0);

  Function* set_null_fn = slot_desc->CodegenUpdateNull(codegen, tuple_type, true);
  if (set_null_fn == NULL) {
    LOG(ERROR) << "Could not codegen WriteSlot because slot update codegen failed.";
    return NULL;
  }

  LlvmCodeGen::FnPrototype prototype(
      codegen, "WriteSlot", codegen->GetType(TYPE_BOOLEAN));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("tuple_arg", tuple_ptr_type));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("data", codegen->ptr_type()));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("len", codegen->GetType(TYPE_INT)));

  LlvmCodeGen::LlvmBuilder builder(codegen->context());
  Value* args[3];
  Function* fn = prototype.GeneratePrototype(&builder, &args[0]);

  // If len == 0, set slot to NULL
  BasicBlock* set_null_block, *parse_slot_block;
  codegen->CreateIfElseBlocks(fn, "set_null", "parse_slot", 
      &set_null_block, &parse_slot_block);
  Value* len_zero = builder.CreateICmpEQ(args[2], codegen->GetIntConstant(TYPE_INT, 0));
  builder.CreateCondBr(len_zero, set_null_block, parse_slot_block);

  // Codegen parse slot block
  builder.SetInsertPoint(parse_slot_block);
  Value* slot = builder.CreateStructGEP(args[0], slot_desc->field_idx(), "slot");

  if (slot_desc->type() == TYPE_STRING) {
    Value* ptr = builder.CreateStructGEP(slot, 0, "string_ptr");
    Value* len = builder.CreateStructGEP(slot, 1, "string_len");
    builder.CreateStore(args[1], ptr);
    builder.CreateStore(args[2], len);
    builder.CreateRet(codegen->true_value());
  } else {
    IRFunction::Type parse_fn_enum;
    Function* parse_fn = NULL;
    switch (slot_desc->type()) {
      case TYPE_BOOLEAN:
        parse_fn_enum = IRFunction::STRING_TO_BOOL;
        break;
      case TYPE_TINYINT:
        parse_fn_enum = IRFunction::STRING_TO_INT8;
        break;
      case TYPE_SMALLINT: 
        parse_fn_enum = IRFunction::STRING_TO_INT16;
        break;
      case TYPE_INT: 
        parse_fn_enum = IRFunction::STRING_TO_INT32;
        break;
      case TYPE_BIGINT:
        parse_fn_enum = IRFunction::STRING_TO_INT64;
        break;
      case TYPE_FLOAT:
        parse_fn_enum = IRFunction::STRING_TO_FLOAT;
        break;
      case TYPE_DOUBLE:
        parse_fn_enum = IRFunction::STRING_TO_DOUBLE;
        break;
      default:
        DCHECK(false);
        return NULL;
    }
    parse_fn = codegen->GetFunction(parse_fn_enum);
    DCHECK(parse_fn != NULL);

    // Set up trying to parse the string to the slot type
    BasicBlock* parse_success_block, *parse_failed_block;
    codegen->CreateIfElseBlocks(fn, "parse_success", "parse_fail",
        &parse_success_block, &parse_failed_block);
    LlvmCodeGen::NamedVariable parse_result("parse_result", codegen->GetType(TYPE_INT));
    Value* parse_result_ptr = codegen->CreateEntryBlockAlloca(fn, parse_result);
    Value* failed_value = codegen->GetIntConstant(TYPE_INT, StringParser::PARSE_FAILURE);

    // Call Impala's StringTo* function
    Value* result = builder.CreateCall3(parse_fn, args[1], args[2], parse_result_ptr);
    Value* parse_result_val = builder.CreateLoad(parse_result_ptr, "parse_result");

    // Check for parse error.  TODO: handle overflow
    Value* parse_failed = builder.CreateICmpEQ(parse_result_val, failed_value, "failed");
    builder.CreateCondBr(parse_failed, parse_failed_block, parse_success_block);
    
    // Parse succeeded
    builder.SetInsertPoint(parse_success_block);
    builder.CreateStore(result, slot);
    builder.CreateRet(codegen->true_value());

    // Parse failed, set slot to null and return false
    builder.SetInsertPoint(parse_failed_block);
    builder.CreateCall(set_null_fn, args[0]);
    builder.CreateRet(codegen->false_value());
  }

  // Case where len == 0
  builder.SetInsertPoint(set_null_block);
  builder.CreateCall(set_null_fn, args[0]);
  builder.CreateRet(codegen->true_value());

  return codegen->FinalizeFunction(fn);
}

