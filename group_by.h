#ifndef GROUP_BY_H
#define GROUP_BY_H

#include <arrow/api.h>
#include <unordered_map>

#include <print.h>
#include <util.h>

//++++++++++++++++++++++++++++++
// GROUP BY
//++++++++++++++++++++++++++++++

// For both single and multiple columns
struct group {
	int max_index;
	std::vector<std::vector<int>> redirection;
	std::vector<std::shared_ptr<arrow::Column>> columns;
	std::vector<std::shared_ptr<arrow::Field>> fields;
	group(): max_index(0) {}
};

// For multiple columns
struct position {
	int row_index; // TODO like in sort? without array?
	std::vector<std::shared_ptr<arrow::Array>> *arrays;
	bool operator==(const position& other) const {
		for (int i = 0; i < (*arrays).size(); i++) {
			// RangeEquals doesn't work
			//if ((*arrays)[i]->RangeEquals(row_index, row_index+1, other.row_index, (*other.arrays)[i]) == false) {
			if (compare((*arrays)[i], row_index, (*other.arrays)[i], other.row_index) != 0) {
				return false;
			}
		}
		return true;
	}
};

namespace std {
	template <>
	// TODO user defined hash function
	struct hash<position> {
		size_t operator()(const position& p) const { // TODO type
			// Compute individual hash values for two data members and combine them using XOR and bit shifting
			size_t answer = 0;
			for (int i = 0; i < (*p.arrays).size(); i++) { // TODO predefine type somehow
				if ((*p.arrays)[i]->type_id() == arrow::Type::STRING) {
					auto array = std::static_pointer_cast<arrow::StringArray>((*p.arrays)[i]);
					answer ^= hash<std::string>()(array->GetString(p.row_index));
				} else { // TODO for double type
					auto array = std::static_pointer_cast<arrow::Int64Array>((*p.arrays)[i]);
					answer ^= hash<int64_t>()(array->Value(p.row_index));
					//TODO answer ^= ((hash<float>()(k.getM()) ^ (hash<float>()(k.getC()) << 1)) >> 1);
				}
			}
			return answer;
		}
	};
};

struct partial_mult_group {
	group *g;
	std::unordered_map<position, int> map;
};

void group_by_sequential_multiple(std::vector<std::shared_ptr<arrow::Array>> *arrays, partial_mult_group* pg, int n) {
	for (int i = 0; i < (*arrays)[0]->length(); i++) {
		position p{i, arrays}; // TODO copy constructor? move constructor?
		auto number = pg->map.find(p);
		if (number != pg->map.end()) {
			pg->g->redirection[n][i] = number->second;
		} else {
			pg->g->redirection[n][i] = pg->map.size();
			pg->map.insert({p, pg->map.size()});
		}
	}
}

group* group_by_parallel_multiple(std::shared_ptr<arrow::Table> table, std::vector<int> column_ids) {
	printf("      Arrow is columnar database and this request is low performance\n");
	printf("      There are two variants: prebuild caches or not. Executing _without_ prebuilding\n");
	partial_mult_group pg = {new(group)};
	// Can different columns have different chunk number? Or it is property of table?
	int num_chunks = table->column(column_ids[0])->data()->num_chunks();

	pg.g->redirection = std::vector<std::vector<int>>(num_chunks);
	for (int i = 0; i < num_chunks; i++) {
		pg.g->redirection[i] = std::vector<int>(table->column(column_ids[0])->data()->chunk(i)->length());
	}

	std::vector<std::vector<std::shared_ptr<arrow::Array>>> all_arrays(num_chunks);
	for (int i = 0; i < num_chunks; i++) {
		for (int j = 0; j < column_ids.size(); j++) {
			all_arrays[i].push_back(table->column(column_ids[j])->data()->chunk(i));
		}
		// TBB in parallel for all available chunks or sequential for each incoming chunk
		group_by_sequential_multiple(&(all_arrays[i]), &pg, i);
	}
	pg.g->max_index = pg.map.size();

	for (int i = 0; i < column_ids.size(); i++) {
		std::shared_ptr<arrow::ChunkedArray> ca = table->column(column_ids[i])->data();
		std::shared_ptr<arrow::Array> data;
		if (ca->type()->id() == arrow::Type::STRING) {
			std::vector<std::string> new_column(pg.map.size());
			for (auto j = pg.map.begin(); j != pg.map.end(); j++) {
				new_column[j->second] = (std::static_pointer_cast<arrow::StringArray>((*j->first.arrays)[i]))->GetString(j->first.row_index);
			}
			data = vector_to_array<std::string, arrow::StringBuilder>(new_column);
		} else if (ca->type()->id() == arrow::Type::INT64) {
			std::vector<int64_t> new_column(pg.map.size());
			for (auto j = pg.map.begin(); j != pg.map.end(); j++) {
				new_column[j->second] = (std::static_pointer_cast<arrow::Int64Array>((*j->first.arrays)[i]))->Value(j->first.row_index);
			}
			data = vector_to_array<arrow::Int64Type::c_type, arrow::Int64Builder>(new_column);
		} else {
			std::vector<double> new_column(pg.map.size());
			for (auto j = pg.map.begin(); j != pg.map.end(); j++) {
				new_column[j->second] = (std::static_pointer_cast<arrow::DoubleArray>((*j->first.arrays)[i]))->Value(j->first.row_index);
			}
			data = vector_to_array<arrow::DoubleType::c_type, arrow::DoubleBuilder>(new_column);
		}
		std::shared_ptr<arrow::Field> field = table->schema()->field(column_ids[i]);
		pg.g->columns.push_back(std::make_shared<arrow::Column>(field->name(), data));
		pg.g->fields.push_back(field);
	}
	return pg.g;
}

// For single column
template <typename T, typename T4>
struct partial_single_group {
	group *g;
	std::unordered_map<T, int> map;
};

template <typename T, typename T2, typename T4>
void group_by_sequential_single(std::shared_ptr<T2> array, partial_single_group<T, T4>* pg) {
	int s = pg->g->redirection.size();
	pg->g->redirection.push_back(std::vector<int>(0));
	for (int i = 0; i < array->length(); i++) {
		T value = get_value<T, T2>(array, i);
		auto number = pg->map.find(value);
		if (number != pg->map.end()) {
			pg->g->redirection[s].push_back(number->second);
		} else {
			pg->g->redirection[s].push_back(pg->map.size());
			pg->map.insert({value, pg->map.size()});
		}
	}
}

template <typename T, typename T2, typename T4>
group* group_by_parallel_single(std::shared_ptr<arrow::Column> column) {
	partial_single_group<T, T4> pg = {new(group)};
	for (int i = 0; i < column->data()->num_chunks(); i++) {
		auto array = std::static_pointer_cast<T2>(column->data()->chunk(i));
		// TBB in parallel for all available chunks or sequential for each incoming chunk
		group_by_sequential_single<T, T2, T4>(array, &pg);
	}
	pg.g->max_index = pg.map.size();

	std::shared_ptr<arrow::Array> data;
	std::vector<T> new_column(pg.map.size());
	for (auto j = pg.map.begin(); j != pg.map.end(); j++) {
		new_column[j->second] = j->first;
	}
	data = vector_to_array<T, T4>(new_column);
        std::shared_ptr<arrow::Field> field = column->field();
        pg.g->columns.push_back(std::make_shared<arrow::Column>(field->name(), data));
        pg.g->fields.push_back(field);

	return pg.g;
}

group* group_by_dispatch(std::shared_ptr<arrow::Table> table, int column_id) {
	std::shared_ptr<arrow::Column> column = table->column(column_id);
	if (column->type()->id() == arrow::Type::STRING) {
		return group_by_parallel_single<std::string, arrow::StringArray, arrow::StringBuilder>(column);
	} else if (column->type()->id() == arrow::Type::INT64) {
		return group_by_parallel_single<arrow::Int64Type::c_type, arrow::Int64Array, arrow::Int64Builder>(column);
	} else {
		return group_by_parallel_single<arrow::DoubleType::c_type, arrow::DoubleArray, arrow::DoubleBuilder>(column);
	}
}

// Main function
group* group_by(std::shared_ptr<arrow::Table> table, std::vector<int> column_ids) {
	printf("TASK: grouping by %s. (building all group_by columns and NOT counting them)\n", column_ids.size() == 1 ? "single column" : "multiple columns");
	auto begin = std::chrono::steady_clock::now();
	group *g;
	if (column_ids.size() == 1) {
		g = group_by_dispatch(table, column_ids[0]);
	} else  {
		g = group_by_parallel_multiple(table, column_ids);
	}
	print_time(begin);
	return g;
}

#endif
