#ifndef PROCESSING_MACRODEF_PROCESSING_INFO_H
#define PROCESSING_MACRODEF_PROCESSING_INFO_H

#include "../../context/macro.h"

namespace hlasm_plugin {
namespace parser_library {
namespace processing {

//data to start macrodef_processor
struct macrodef_start_data 
{
	bool is_external;
	context::id_index external_name;
	
	macrodef_start_data() :is_external(false), external_name(context::id_storage::empty_id) {}
	macrodef_start_data(context::id_index external_name) :is_external(true), external_name(external_name) {}
};

//data holding info about prototype statement of a macro
struct macrodef_prototype
{
	macrodef_prototype()
		:macro_name(context::id_storage::empty_id), name_param(context::id_storage::empty_id) {}

	context::id_index macro_name;

	context::id_index name_param;
	std::vector<context::macro_arg> symbolic_params;
};

//result of macrodef_processor
struct macrodef_processing_result
{
	macrodef_prototype prototype;

	context::statement_block definition;
	context::copy_nest_storage nests;
	context::label_storage sequence_symbols;

	location definition_location;
};

}
}
}
#endif