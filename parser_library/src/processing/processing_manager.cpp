#include "processing_manager.h"
#include "statement_processors/macrodef_processor.h"
#include "statement_processors/lookahead_processor.h"
#include "statement_processors/ordinary_processor.h"
#include "statement_processors/copy_processor.h"
#include "statement_processors/empty_processor.h"
#include "macro_statement_provider.h"
#include "copy_statement_provider.h"
#include "../parser_impl.h"
#include "../ebcdic_encoding.h"

#include <assert.h>

using namespace hlasm_plugin::parser_library;
using namespace hlasm_plugin::parser_library::processing;

processing_manager::processing_manager(
	std::unique_ptr<opencode_provider> base_provider,
	context::hlasm_context& hlasm_ctx,
	const library_data data,
	parse_lib_provider& lib_provider, 
	statement_fields_parser& parser,
	processing_tracer* tracer)
	: diagnosable_ctx(hlasm_ctx), hlasm_ctx_(hlasm_ctx), lib_provider_(lib_provider),
	opencode_prov_(*base_provider), tracer_(tracer)
	
{

	switch (data.proc_kind)
	{
	case processing_kind::ORDINARY:
		provs_.emplace_back(std::make_unique<macro_statement_provider>(hlasm_ctx, parser));
		procs_.emplace_back(std::make_unique<ordinary_processor>(hlasm_ctx, *this, *this, lib_provider, *this, parser, tracer_));
		break;
	case processing_kind::COPY:
		procs_.emplace_back(std::make_unique<copy_processor>(hlasm_ctx, *this, copy_start_data{ data.library_member }));
		break;
	case processing_kind::MACRO:
		procs_.emplace_back(std::make_unique<macrodef_processor>(hlasm_ctx, *this,lib_provider, macrodef_start_data(data.library_member)));
		break;
	default:
		break;
	}

	provs_.emplace_back(std::make_unique<copy_statement_provider>(hlasm_ctx, parser));
	provs_.emplace_back(std::move(base_provider));
}

void processing_manager::start_processing(std::atomic<bool>* cancel)
{
	while (!procs_.empty())
	{
		if (cancel && *cancel)
			break;

		statement_processor& proc = *procs_.back();
		statement_provider& prov = find_provider();

		if ((prov.finished() && proc.terminal_condition(prov.kind)) ||
			proc.finished())
		{
			finish_processor();
			continue;
		}
		
		prov.process_next(proc);
	}
	if (!cancel || !*cancel)
		add_ord_sym_defs();
}

statement_provider& processing_manager::find_provider()
{
	for (auto& prov : provs_) {
		if (!prov->finished())
			return *prov;
	}

	return *provs_.back();
}

void processing_manager::finish_processor()
{
	procs_.back()->end_processing();
	collect_diags_from_child(*procs_.back());
	procs_.pop_back();
}

void processing_manager::start_macro_definition(const macrodef_start_data start)
{
	hlasm_ctx_.push_statement_processing(processing_kind::MACRO);
	procs_.emplace_back(std::make_unique<macrodef_processor>(hlasm_ctx_, *this, lib_provider_, start));
}

void processing_manager::finish_macro_definition(macrodef_processing_result result)
{
	if(!result.invalid)
		hlasm_ctx_.add_macro(
			result.prototype.macro_name,
			result.prototype.name_param,
			std::move(result.prototype.symbolic_params),
			std::move(result.definition),
			std::move(result.nests),
			std::move(result.sequence_symbols),
			std::move(result.definition_location));
}

void processing_manager::start_lookahead(lookahead_start_data start)
{
	hlasm_ctx_.push_statement_processing(processing_kind::LOOKAHEAD);
	procs_.emplace_back(std::make_unique<lookahead_processor>(hlasm_ctx_, *this, *this, lib_provider_, std::move(start)));
}

void processing_manager::finish_lookahead(lookahead_processing_result result)
{
	if (result.success)
		jump_in_statements(result.symbol_name,result.symbol_range);
	else
	{
		perform_opencode_jump(result.statement_position, std::move(result.snapshot));

		empty_processor tmp(hlasm_ctx_); //skip next statement
		find_provider().process_next(tmp);

		add_diagnostic(diagnostic_op::error_E047(*result.symbol_name, result.symbol_range));
	}
}

void processing_manager::start_copy_member(copy_start_data start)
{
	procs_.emplace_back(std::make_unique < copy_processor>(hlasm_ctx_, *this, std::move(start)));
}

void processing_manager::finish_copy_member(copy_processing_result result)
{
	hlasm_ctx_.add_copy_member(
		result.member_name, 
		result.invalid_member ? context::statement_block() : std::move(result.definition),
		std::move(result.definition_location));
}

void processing_manager::jump_in_statements(context::id_index target, range symbol_range)
{
	auto symbol = hlasm_ctx_.get_sequence_symbol(target);
	if (!symbol)
	{
		if (hlasm_ctx_.is_in_macro())
		{
			add_diagnostic(diagnostic_op::error_E047(*target, symbol_range));
		}
		else
		{
			auto open_symbol = create_opencode_sequence_symbol(nullptr, range());
			start_lookahead(lookahead_start_data(
				target,
				symbol_range,
				std::move(open_symbol->statement_position),
				std::move(open_symbol->snapshot)
				));
		}
			
	}
	else
	{
		if (symbol->kind == context::sequence_symbol_kind::MACRO)
		{
			assert(hlasm_ctx_.is_in_macro());
			hlasm_ctx_.scope_stack().back().this_macro->current_statement = (int)symbol->access_macro_symbol()->statement_offset - 1;
		}
		else
		{
			auto opencode_symbol = symbol->access_opencode_symbol();

			perform_opencode_jump(opencode_symbol->statement_position,opencode_symbol->snapshot);
		}

		hlasm_ctx_.decrement_branch_counter();
	}
}

void processing_manager::register_sequence_symbol(context::id_index target, range symbol_range)
{
	if (hlasm_ctx_.is_in_macro())
		return;

	auto symbol = hlasm_ctx_.get_sequence_symbol(target);
	auto new_symbol = create_opencode_sequence_symbol(target, symbol_range);

	if (!symbol)
	{
		hlasm_ctx_.add_sequence_symbol(std::move(new_symbol));
	}
	else if(!(*symbol->access_opencode_symbol() == *new_symbol))
	{
		add_diagnostic(diagnostic_op::error_E045(*target, symbol_range));
	}
}

std::unique_ptr<context::opencode_sequence_symbol> processing_manager::create_opencode_sequence_symbol(context::id_index name, range symbol_range)
{
	auto symbol_pos = symbol_range.start;
	location loc(symbol_pos, hlasm_ctx_.processing_stack().back().proc_location.file);

	context::source_position statement_position((size_t)hlasm_ctx_.current_source().source_status.pos.line, 0);

	if (hlasm_ctx_.current_source().copy_stack.empty())
		statement_position.file_offset = hlasm_ctx_.current_source().begin_index;
	else
	{
		statement_position.file_offset = hlasm_ctx_.current_source().end_index;
		++statement_position.file_line;
	}

	auto snapshot = hlasm_ctx_.current_source().create_snapshot();

	return std::make_unique<context::opencode_sequence_symbol>(name, loc, statement_position, std::move(snapshot));
}

void processing_manager::perform_opencode_jump(context::source_position statement_position, context::source_snapshot snapshot)
{
	opencode_prov_.rewind_input(statement_position);

	hlasm_ctx_.apply_source_snapshot(std::move(snapshot));
}

void hlasm_plugin::parser_library::processing::processing_manager::add_ord_sym_defs()
{
	// for all collected ordinary symbol definitions
	for (const auto& occurence : hlasm_ctx_.lsp_ctx->deferred_ord_defs)
	{
		auto definition = occurence;
		// symbol not in ordinary context, skip it
		if (!create_sym_def(definition))
			continue;

		definition.definition_range = occurence.definition_range;
		//add itself
		hlasm_ctx_.lsp_ctx->ord_symbols.insert({ definition, { definition.definition_range,definition.file_name } });
		// adds all its occurences
		for (const auto& deferred_sym : hlasm_ctx_.lsp_ctx->deferred_ord_occs)
			if (deferred_sym == occurence)
				hlasm_ctx_.lsp_ctx->ord_symbols.insert({ definition, { deferred_sym.definition_range,deferred_sym.file_name } });
		hlasm_ctx_.lsp_ctx->deferred_ord_occs.erase(std::remove(hlasm_ctx_.lsp_ctx->deferred_ord_occs.begin(), hlasm_ctx_.lsp_ctx->deferred_ord_occs.end(), occurence), hlasm_ctx_.lsp_ctx->deferred_ord_occs.end());
	}

	// if there are still some symbols in occurences, check if they are defined in context
	for (const auto& occurence : hlasm_ctx_.lsp_ctx->deferred_ord_occs)
	{
		auto definition = occurence;
		// symbol not in ordinary context, skip it
		if (!create_sym_def(definition))
			continue;
		hlasm_ctx_.lsp_ctx->ord_symbols.insert({ definition, { occurence.definition_range,occurence.file_name } });
	}
}

bool hlasm_plugin::parser_library::processing::processing_manager::create_sym_def(context::definition & definition)
{
	// get the symbol id
	auto id = hlasm_ctx_.ids().find(definition.name);
	// get the definition from the ordinary context
	auto symbol = hlasm_ctx_.ord_ctx.get_symbol(id);
	if (symbol)
	{
		//set file range
		definition.definition_range = { symbol->symbol_location.pos,symbol->symbol_location.pos };
		definition.file_name = symbol->symbol_location.file;
		// extract its value
		auto val = symbol->value();
		if (val.value_kind() == context::symbol_value_kind::ABS)
		{
			definition.value = { std::to_string(val.get_abs()) };
			definition.value.push_back("Absolute Symbol");
		}
		else if (val.value_kind() == context::symbol_value_kind::RELOC)
		{
			definition.value = { val.get_reloc().to_string() };
			definition.value.push_back("Relocatable Symbol");
		}
		//extract its attributes
		if (symbol->attributes().is_defined(context::data_attr_kind::L))
			definition.value.push_back("L: " + std::to_string(symbol->attributes().get_attribute_value(context::data_attr_kind::L)));
		if (symbol->attributes().is_defined(context::data_attr_kind::I))
			definition.value.push_back("I: " + std::to_string(symbol->attributes().get_attribute_value(context::data_attr_kind::I)));
		if (symbol->attributes().is_defined(context::data_attr_kind::S))
			definition.value.push_back("S: " + std::to_string(symbol->attributes().get_attribute_value(context::data_attr_kind::S)));
		if (symbol->attributes().is_defined(context::data_attr_kind::T))
			definition.value.push_back("T: " + ebcdic_encoding::to_ascii((unsigned char)symbol->attributes().get_attribute_value(context::data_attr_kind::T)));
		return true;
	}
	return false;
}

attribute_provider::resolved_reference_storage processing_manager::lookup_forward_attribute_references(attribute_provider::forward_reference_storage references)
{
	if (references.empty())
		return {};

	lookahead_processor proc(hlasm_ctx_, *this, *this, lib_provider_, lookahead_start_data(std::move(references)));

	context::source_snapshot snapshot = hlasm_ctx_.current_source().create_snapshot();
	if (!snapshot.copy_frames.empty())
		++snapshot.copy_frames.back().statement_offset;

	context::source_position statement_position(
		(size_t)hlasm_ctx_.current_source().source_status.pos.line + 1, 
		hlasm_ctx_.current_source().end_index);

	opencode_prov_.push_line_end();

	while (true)
	{
		//macro statement provider is not relevant in attribute lookahead
		//provs_.size() is always more than 2, it results from calling constructor
		auto& opencode_prov = **(provs_.end() - 1);
		auto& copy_prov = **(provs_.end() - 2);
		auto& prov = !copy_prov.finished() ? copy_prov : opencode_prov;

		if (prov.finished() || proc.finished())
			break;

		prov.process_next(proc);
	}

	perform_opencode_jump(statement_position, std::move(snapshot));

	return proc.collect_found_refereces();
}

void processing_manager::collect_diags() const
{
	for (auto& proc : procs_)
		collect_diags_from_child(*proc);

	collect_diags_from_child(dynamic_cast<parser_impl&>(*provs_.back()));
}
