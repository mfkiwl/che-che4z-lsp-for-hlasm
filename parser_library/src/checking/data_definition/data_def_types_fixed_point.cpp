#include "data_def_types.h"

#include "../checker_helper.h"

using namespace hlasm_plugin::parser_library::checking;
using namespace hlasm_plugin::parser_library::context;
using namespace hlasm_plugin::parser_library;

//***************************   types H, F, FD   *****************************//

data_def_type_H_F_FD::data_def_type_H_F_FD(char type, char extension, uint8_t word_length) :
	data_def_type(type, extension, modifier_bound{ 1, 64 }, modifier_bound{ 1, 8 },
		modifier_bound{ -187 , 346 }, modifier_bound{ -85 , 75 }, nominal_value_type::STRING, {0, word_length}, word_length),
	word_length_(word_length) {}

class H_F_FD_number_spec
{
public:
	static bool is_end_char(char c)
	{
		return c == ',' || c == 'E';
	}

	static bool is_sign_char(char c)
	{
		return c == 'U' || c == '+' || c == '-';
	}
};

bool data_def_type_H_F_FD::check(const data_definition_operand& op, const diagnostic_collector& add_diagnostic, bool check_nominal) const
{
	if (!check_nominal)
		return true;

	size_t i = 0;
	const std::string& nom = std::get<std::string>(op.nominal_value.value);
	if (nom.size() == 0)
	{
		add_diagnostic(diagnostic_op::error_D010(op.nominal_value.rng, type_str));
		return false;
	}
	while (i < nom.size())
	{
		//checks number, may begin with +,- or U, ends with exponent or comma
		if (!check_number<H_F_FD_number_spec>(nom, i))
		{
			add_diagnostic(diagnostic_op::error_D010(op.nominal_value.rng, type_str));
			return false;
		}
		if (i >= nom.size())
			return true;
		//check exponent
		if (nom[i] == 'E')
		{
			if (!check_exponent(nom, i))
			{
				add_diagnostic(diagnostic_op::error_D010(op.nominal_value.rng, type_str));
				return false;
			}
			if (i >= nom.size())
				return true;
		}
		if (nom[i] != ',')
		{
			add_diagnostic(diagnostic_op::error_D010(op.nominal_value.rng, type_str));
			return false;
		}
		++i;
	}
	if (nom.back() == ',')
	{
		add_diagnostic(diagnostic_op::error_D010(op.nominal_value.rng, type_str));
		return false;
	}

	return true;
	//TODO truncation is also an error

}

data_def_type_H::data_def_type_H() : data_def_type_H_F_FD('H', '\0', 2) {}

data_def_type_F::data_def_type_F() : data_def_type_H_F_FD('F', '\0', 4) {}

data_def_type_FD::data_def_type_FD() : data_def_type_H_F_FD('F', 'D', 8) {}

//***************************   types P, Z   *****************************//

class P_Z_number_spec
{
public:
	static bool is_end_char(char c)
	{
		return c == ',';
	}

	static bool is_sign_char(char c)
	{
		return c == '+' || c == '-';
	}
};

data_def_type_P_Z::data_def_type_P_Z(char type) :
	data_def_type(type, '\0', modifier_bound{ 1, 128 }, modifier_bound{ 1, 16 },
		n_a(), n_a(), nominal_value_type::STRING, no_align, as_needed()) {}

bool data_def_type_P_Z::check(const data_definition_operand& op, const diagnostic_collector& add_diagnostic, bool check_nominal) const
{
	//TO DO truncation is also an error
	if (!check_nominal)
		return true;

	size_t i = 0;
	const std::string& nom = std::get<std::string>(op.nominal_value.value);
	if (nom.size() == 0)
	{
		add_diagnostic(diagnostic_op::error_D010(op.nominal_value.rng, type_str));
		return false;
	}
	while (i < nom.size())
	{
		if (!check_number<P_Z_number_spec>(nom, i))
		{
			add_diagnostic(diagnostic_op::error_D010(op.nominal_value.rng, type_str));
			return false;
		}
		++i;
	}
	if (nom.back() == ',')
	{
		add_diagnostic(diagnostic_op::error_D010(op.nominal_value.rng, type_str));
		return false;
	}

	return true;
}

data_def_type_P::data_def_type_P() : data_def_type_P_Z('P') {}

uint64_t data_def_type_P::get_nominal_length(const nominal_value_t& op) const
{
	if (!op.present)
		return 1;

	const std::string& s = std::get<std::string>(op.value);
	
	uint64_t bytes_count = 0;
	//4 sign bits are added to each assembled number
	uint64_t halfbytes_count = 1;
	for (char c : s)
	{
		if (c == ',')
		{
			bytes_count += (halfbytes_count + 1) / 2;
			halfbytes_count = 1;
		}
		else if (is_digit(c))
			++halfbytes_count;
	}
	bytes_count += (halfbytes_count + 1) / 2;
	//each digit is assembled as 4 bits, 4 more sign bits are assembled per each number

	return bytes_count;
}

data_def_type_Z::data_def_type_Z() : data_def_type_P_Z('Z') {}

uint64_t data_def_type_Z::get_nominal_length(const nominal_value_t& op) const
{
	if (!op.present)
		return 1;
	
	const std::string& s = std::get<std::string>(op.value);

	//each digit is assembled as one byte

	return std::count_if(s.cbegin(), s.cend(), &is_digit);
}