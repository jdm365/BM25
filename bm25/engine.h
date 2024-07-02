#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <mutex>

#include "robin_hood.h"
#include "bloom.h"


#define DEBUG 0

#define SEED 42


enum SupportedFileTypes {
	CSV,
	JSON,
	IN_MEMORY
};


struct _compare {
	inline bool operator()(const std::pair<uint32_t, float>& a, const std::pair<uint32_t, float>& b) {
		return a.second > b.second;
	}
};

struct _compare_64 {
	inline bool operator()(const std::pair<uint64_t, float>& a, const std::pair<uint64_t, float>& b) {
		return a.second > b.second;
	}
};

struct _compare_64_16 {
	inline bool operator()(const std::pair<uint64_t, uint16_t>& a, const std::pair<uint64_t, uint16_t>& b) {
		return a.second > b.second;
	}
};




typedef struct {
	uint64_t df;
	std::vector<uint64_t> doc_ids;
	std::vector<uint16_t> term_freqs;
} IIRow;

typedef struct {
	uint64_t doc_id;
	float    score;
	uint16_t partition_id;
} BM25Result;

struct _compare_bm25_result {
	inline bool operator()(const BM25Result& a, const BM25Result& b) {
		return a.score > b.score;
	}
};



typedef struct {
	uint16_t num_repeats;
	uint8_t  value;
} RLEElement_u8;

RLEElement_u8 init_rle_element_u8(uint8_t value);
uint64_t get_rle_element_u8_size(const RLEElement_u8& rle_element);
bool check_rle_u8_row_size(const std::vector<RLEElement_u8>& rle_row, uint64_t max_size);
void add_rle_element_u8(std::vector<RLEElement_u8>& rle_row, uint8_t value);



typedef struct BloomEntry {
	robin_hood::unordered_flat_map<uint16_t, BloomFilter> bloom_filters;
	std::vector<uint64_t> topk_doc_ids;
	std::vector<float> topk_term_freqs;
} BloomEntry;

BloomEntry init_bloom_entry(
		double fpr, 
		robin_hood::unordered_flat_map<uint16_t, uint64_t>& tf_map 
		);

typedef struct {
	std::vector<uint8_t> doc_ids;
	std::vector<RLEElement_u8> term_freqs;
} StandardEntry;

typedef struct {
	std::vector<uint64_t> prev_doc_ids;
	std::vector<uint32_t> doc_freqs;
	std::vector<StandardEntry> inverted_index_compressed;
	robin_hood::unordered_flat_map<uint64_t, BloomEntry> bloom_filters;
} InvertedIndex;

inline IIRow get_II_row(InvertedIndex* II, uint64_t term_idx);

typedef struct {
	std::vector<InvertedIndex> II;
	std::vector<robin_hood::unordered_flat_map<std::string, uint32_t>> unique_term_mapping;
	std::vector<uint16_t> doc_sizes;
	std::vector<uint64_t> line_offsets;

	uint64_t num_docs;
	float    avg_doc_size;

	// Debug reverse term mapping
	std::vector<robin_hood::unordered_flat_map<uint32_t, std::string>> reverse_term_mapping;
} BM25Partition;


class _BM25 {
	public:
		std::vector<BM25Partition> index_partitions;
		robin_hood::unordered_flat_set<std::string> stop_words;

		uint64_t num_docs;
		float    bloom_df_threshold;
		double   bloom_fpr;
		float    k1;
		float    b;
		uint16_t num_partitions;

		SupportedFileTypes file_type;

		std::vector<std::string> search_cols;
		std::string filename;
		std::vector<std::string> columns;
		std::vector<int16_t> search_col_idxs;
		uint16_t header_bytes;

		std::vector<uint64_t> partition_boundaries;

		std::vector<FILE*> reference_file_handles;

		std::vector<std::string> progress_bars;
		std::mutex progress_mutex;
		int init_cursor_row;
		int terminal_height;


		_BM25(
				std::string filename,
				std::vector<std::string> search_cols,
				float  bloom_df_threshold,
				double bloom_fpr,
				float  k1,
				float  b,
				uint16_t num_partitions,
				const std::vector<std::string>& _stop_words = {}
				);

		_BM25(std::string db_dir) {
			load_from_disk(db_dir);

			// filename found in db_dir/filename.txt
			std::string fn_file = db_dir + "/filename.txt";

			// read fn_file contents into filename 
			FILE* f = fopen(fn_file.c_str(), "r");
			if (f == nullptr) {
				std::cerr << "Error opening file: " << fn_file << std::endl;
				exit(1);
			}
			char buf[1024];
			fgets(buf, 1024, f);
			fclose(f);
			filename = std::string(buf);

			if (filename == "in_memory") {
				file_type = IN_MEMORY;
			} else if (filename.find(".json") != std::string::npos) {
				file_type = JSON;
			} else if (filename.find(".csv") != std::string::npos) {
				file_type = CSV;
			} else {
				std::cerr << "Error: file type not supported" << std::endl;
				exit(1);
			}

			if (file_type == IN_MEMORY) {
				return;
			}
			// Open the reference file
			for (uint16_t i = 0; i < num_partitions; i++) {
				FILE* ref_f = fopen(filename.c_str(), "r");
				if (ref_f == nullptr) {
					std::cerr << "Error opening file: " << filename << std::endl;
					exit(1);
				}
				reference_file_handles.push_back(ref_f);
			}
		}

		_BM25(
				std::vector<std::vector<std::string>>& documents,
				float  bloom_df_threshold,
				double bloom_fpr,
				float  k1,
				float  b,
				uint16_t num_partitions,
				const std::vector<std::string>& _stop_words = {}
				);

		~_BM25() {
			for (uint16_t i = 0; i < num_partitions; i++) {
				if (reference_file_handles[i] != nullptr) {
					fclose(reference_file_handles[i]);
				}
			}
		}
		void init_terminal();
		void proccess_csv_header();

		void save_index_partition(std::string db_dir, uint16_t partition_id);
		void load_index_partition(std::string db_dir, uint16_t partition_id);
		void save_to_disk(const std::string& db_dir);
		void load_from_disk(const std::string& db_dir);

		uint32_t process_doc_partition(
				const char* doc,
				const char terminator,
				uint64_t doc_id,
				uint32_t& unique_terms_found,
				uint16_t partition_id,
				uint16_t col_idx
				);
		uint32_t process_doc_partition_rfc_4180(
				const char* doc,
				const char terminator,
				uint64_t doc_id,
				uint32_t& unique_terms_found,
				uint16_t partition_id,
				uint16_t col_idx
				);
		void process_doc_partition_rfc_4180_mmap(
				const char* file_data,
				const char terminator,
				uint64_t doc_id,
				uint32_t& unique_terms_found,
				uint16_t partition_id,
				uint16_t col_idx,
				uint64_t& byte_offset
				);

		void determine_partition_boundaries_csv();
		void determine_partition_boundaries_csv_rfc_4180();
		void determine_partition_boundaries_json();

		void write_bloom_filters(uint16_t partition_id);
		void read_json(uint64_t start_byte, uint64_t end_byte, uint16_t partition_id);
		void read_csv_rfc_4180(uint64_t start_byte, uint64_t end_byte, uint16_t partition_id);
		void read_csv_rfc_4180_mmap(uint64_t start_byte, uint64_t end_byte, uint16_t partition_id);
		void read_in_memory(
				std::vector<std::vector<std::string>>& documents,
				uint64_t start_idx, 
				uint64_t end_idx, 
				uint16_t partition_id
				);
		std::vector<std::pair<std::string, std::string>> get_csv_line(int line_num, uint16_t partition_id);
		std::vector<std::pair<std::string, std::string>> get_json_line(int line_num, uint16_t partition_id);

		void init_dbs();

		void write_row_to_inverted_index_db(
				const std::string& term,
				uint64_t doc_id
				);

		float _compute_bm25(
				uint64_t doc_id,
				float tf,
				float idf,
				uint16_t partition_id
				);

		void add_query_term(
				std::string& substr,
				std::vector<std::vector<uint64_t>>& term_idxs,
				uint16_t partition_id
				);
		void add_query_term_bloom(
				std::string& substr,
				std::vector<std::vector<uint64_t>>& low_df_term_idxs,
				std::vector<std::vector<uint64_t>>& high_df_term_idxs,
				std::vector<std::vector<BloomEntry>>& bloom_entries,
				uint16_t partition_id
				);
		void add_query_term_bloom(
				std::string& substr,
				std::vector<std::vector<uint64_t>>& low_df_term_idxs,
				std::vector<std::vector<uint64_t>>& high_df_term_idxs,
				std::vector<std::vector<BloomEntry>>& bloom_entries,
				uint16_t partition_id,
				uint16_t col_idx
				);
		std::vector<BM25Result> _query_partition(
				std::string& query,
				uint32_t top_k,
				uint32_t query_max_df,
				uint16_t partition_id,
				std::vector<float> boost_factors
				);
		std::vector<BM25Result> _query_partition_bloom(
				std::string& query,
				uint32_t top_k,
				uint32_t query_max_df,
				uint16_t partition_id,
				std::vector<float> boost_factors
				);
		std::vector<BM25Result> _query_partition_streaming(
				std::string& query,
				uint32_t top_k,
				uint32_t query_max_df,
				uint16_t partition_id,
				std::vector<float> boost_factors
				);
		std::vector<BM25Result> query(
				std::string& query,
				uint32_t top_k,
				uint32_t query_max_df,
				std::vector<float> boost_factors
				);

		std::vector<std::vector<std::pair<std::string, std::string>>> get_topk_internal(
				std::string& _query,
				uint32_t top_k,
				uint32_t query_max_df,
				std::vector<float> boost_factors
				);

		std::vector<BM25Result> _query_partition_bloom_multi(
				std::vector<std::string>& query,
				uint32_t k,
				uint32_t query_max_df,
				uint16_t partition_id,
				std::vector<float> boost_factors
				);
		std::vector<BM25Result> query_multi(
				std::vector<std::string>& query,
				uint32_t top_k,
				uint32_t query_max_df,
				std::vector<float> boost_factors
				);
		std::vector<std::vector<std::pair<std::string, std::string>>> get_topk_internal_multi(
				std::vector<std::string>& _query,
				uint32_t top_k,
				uint32_t query_max_df,
				std::vector<float> boost_factors
				);

		void update_progress(int line_num, int num_lines, uint16_t partition_id);
		void finalize_progress_bar();
};
