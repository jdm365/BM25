#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>

#include <iostream>
#include <vector>
#include <algorithm>
#include <queue>
#include <string>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <sstream>

#include <chrono>
#include <ctime>
#include <sys/mman.h>
#include <fcntl.h>
#include <omp.h>
#include <thread>
#include <mutex>
#include <termios.h>

#include "engine.h"
#include "robin_hood.h"
#include "vbyte_encoding.h"
#include "serialize.h"


void set_raw_mode() {
    termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag &= ~(ICANON | ECHO); // Disable echo and canonical mode
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}

// Function to reset the terminal to normal mode
void reset_terminal_mode() {
    termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag |= (ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}

// Function to query the cursor position
void get_cursor_position(int &rows, int &cols) {
    set_raw_mode();

    // Send the ANSI code to report cursor position
    std::cout << "\x1b[6n" << std::flush;

    // Expecting response in the format: ESC[row;colR
    char ch;
    int rows_temp = 0, cols_temp = 0;
    int read_state = 0;

    while (std::cin.get(ch)) {
        if (ch == '\x1b') {
            read_state = 1;
        } else if (ch == '[' && read_state == 1) {
            read_state = 2;
        } else if (ch == 'R') {
            break;
        } else if (read_state == 2 && ch != ';') {
            rows_temp = rows_temp * 10 + (ch - '0');
        } else if (ch == ';') {
            read_state = 3;
        } else if (read_state == 3) {
            cols_temp = cols_temp * 10 + (ch - '0');
        }
    }

    reset_terminal_mode();

    rows = rows_temp;
    cols = cols_temp;
}

void get_terminal_size(int &rows, int &cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        perror("ioctl");
        exit(EXIT_FAILURE);
    }
    rows = ws.ws_row;
    cols = ws.ws_col;
}


void _BM25::determine_partition_boundaries_csv(std::vector<uint64_t>& partition_boundaries) {
	// First find number of bytes in file.
	// Get avg chunk size in bytes.
	// Seek in jumps of byte chunks, then scan forward to newline and append to partition_boundaries.
	// If we reach end of file, break.

	FILE* f = reference_file_handles[0];

	struct stat sb;
	if (fstat(fileno(f), &sb) == -1) {
		std::cerr << "Error getting file size." << std::endl;
		std::exit(1);
	}

	size_t file_size = sb.st_size;
	size_t chunk_size = file_size / num_partitions;

	if (max_df < 2.0f) {
		// Guess for now
		this->max_df = (int)file_size * max_df / 100;
	}

	partition_boundaries.push_back(header_bytes);

	size_t byte_offset = header_bytes;
	while (true) {
		byte_offset += chunk_size;

		if (byte_offset >= file_size) {
			partition_boundaries.push_back(file_size);
			break;
		}

		fseek(f, byte_offset, SEEK_SET);

		char buf[1024];
		while (true) {
			size_t bytes_read = fread(buf, 1, sizeof(buf), f);
			for (size_t i = 0; i < bytes_read; ++i) {
				if (buf[i] == '\n') {
					partition_boundaries.push_back(byte_offset++);
					goto end_of_loop;
				}
				++byte_offset;
			}
		}

		end_of_loop:
			continue;
	}

	if (partition_boundaries.size() != num_partitions + 1) {
		printf("Partition boundaries: %lu\n", partition_boundaries.size());
		printf("Num partitions: %d\n", num_partitions);
		std::cerr << "Error determining partition boundaries." << std::endl;
		std::exit(1);
	}

	// Reset file pointer to beginning
	fseek(f, header_bytes, SEEK_SET);
}

void _BM25::determine_partition_boundaries_json(std::vector<uint64_t>& partition_boundaries) {
	// Same as csv for now. Assuming newline delimited json.
	determine_partition_boundaries_csv(partition_boundaries);
}

void _BM25::proccess_csv_header() {
	// Iterate over first line to get column names.
	// If column name matches search_col, set search_column_index.

	FILE* f = reference_file_handles[0];
	char* line = NULL;
	size_t len = 0;

	fseek(f, 0, SEEK_SET);

	// Get col names
	ssize_t read = getline(&line, &len, f);
	std::istringstream iss(line);
	std::string value;
	while (std::getline(iss, value, ',')) {
		if (value.find("\n") != std::string::npos) {
			value.erase(value.find("\n"));
		}
		columns.push_back(value);
		if (value == search_col) {
			search_col_idx = columns.size() - 1;
		}
	}

	if (search_col_idx == -1) {
		std::cerr << "Search column not found in header" << std::endl;
		std::cerr << "Cols found:  ";
		for (size_t i = 0; i < columns.size(); ++i) {
			std::cerr << columns[i] << ",";
		}
		std::cerr << std::endl;
		exit(1);
	}

	header_bytes = read;
}

inline RLEElement_u8 init_rle_element_u8(uint8_t value) {
	RLEElement_u8 rle;
	rle.num_repeats = 1;
	rle.value = value;
	return rle;
}

inline uint64_t get_rle_element_u8_size(const RLEElement_u8& rle_element) {
	return (uint64_t)rle_element.num_repeats;
}

bool check_rle_u8_row_size(const std::vector<RLEElement_u8>& rle_row, uint64_t max_size) {
	uint64_t size = 0;
	for (const auto& rle_element : rle_row) {
		size += get_rle_element_u8_size(rle_element);
		if (size >= max_size) {
			return true;
		}
	}
	return false;
}

inline uint64_t get_rle_u8_row_size(const std::vector<RLEElement_u8>& rle_row) {
	uint64_t size = 0;
	for (const auto& rle_element : rle_row) {
		size += get_rle_element_u8_size(rle_element);
	}
	return size;
}

void add_rle_element_u8(std::vector<RLEElement_u8>& rle_row, uint8_t value) {
	if (rle_row.empty()) {
		rle_row.push_back(init_rle_element_u8(value));
	}
	else {
		if (rle_row.back().value == value) {
			++rle_row.back().num_repeats;
		}
		else {
			rle_row.push_back(init_rle_element_u8(value));
		}
	}
}

/*
void _BM25::process_doc(
		const char* doc,
		const char terminator,
		uint64_t doc_id,
		uint64_t& unique_terms_found
		) {
	uint64_t char_idx = 0;

	std::string term = "";
	// term.reserve(22);

	robin_hood::unordered_flat_set<uint64_t> terms_seen;

	// Split by commas not inside double quotes
	uint64_t doc_size = 0;
	uint8_t tf = 1;
	while (doc[char_idx] != terminator) {
		if (char_idx > 1048576) {
			std::cout << "Search field not found on line: " << doc_id << std::endl;
			std::cout << "Doc: " << doc << std::endl;
			std::cout << std::flush;
			std::exit(1);
		}
		if (doc[char_idx] == '\\') {
			++char_idx;
			term += toupper(doc[char_idx]);
			++char_idx;
			continue;
		}

		if (doc[char_idx] == ' ' && term == "") {
			++char_idx;
			continue;
		}

		if (doc[char_idx] == ' ') {
			if (stop_words.find(term) != stop_words.end()) {
				term.clear();
				++char_idx;
				++doc_size;
				continue;
			}

			auto [it, add] = unique_term_mapping.try_emplace(term, unique_terms_found);
			if (add) {
				// New term
				// II.accumulator.push_back(entry);
				// convert uint64_t bytes to uint8_t bytes
				
				InvertedIndexElement entry;

				compress_uint64_differential_single(entry.doc_ids, doc_id, 0);
				entry.term_freqs.push_back(init_rle_element_u8(tf));

				II.inverted_index_compressed.push_back(entry);
				II.prev_doc_ids.push_back(doc_id);

				terms_seen.insert(it->second);

				++unique_terms_found;
			}
			else {
				if (check_rle_u8_row_size(II.inverted_index_compressed[it->second].term_freqs, max_df)) {
					if (II.inverted_index_compressed[it->second].doc_ids.size() >= 0) {
						II.inverted_index_compressed[it->second].doc_ids.clear();
						II.inverted_index_compressed[it->second].doc_ids.shrink_to_fit();
					}

					// Skip term
					term.clear();
					++char_idx;
					++doc_size;
					continue;
				}

				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert(it->second);

					bool same = compress_uint64_differential_single(
							II.inverted_index_compressed[it->second].doc_ids, 
							doc_id,
							II.prev_doc_ids[it->second]
							);
					II.prev_doc_ids[it->second] = doc_id;

					if (same) {
						// ++II.inverted_index_compressed[it->second].term_freqs.back();
						++tf;
					}
					else {
						add_rle_element_u8(II.inverted_index_compressed[it->second].term_freqs, tf);
						// II.inverted_index_compressed[it->second].term_freqs.push_back(1);
						tf = 1;
					}
				}
			}

			++doc_size;
			term.clear();

			++char_idx;
			continue;
		}

		term += toupper(doc[char_idx]);
		++char_idx;
	}

	if (term != "") {
		if (stop_words.find(term) == stop_words.end()) {
			auto [it, add] = unique_term_mapping.try_emplace(term, unique_terms_found);

			if (add) {
				// New term
				// II.accumulator.push_back(entry);
				// convert uint64_t bytes to uint8_t bytes
				InvertedIndexElement entry;

				compress_uint64_differential_single(entry.doc_ids, doc_id, 0);
				// entry.term_freqs.push_back(1);
				entry.term_freqs.push_back(init_rle_element_u8(tf));
				tf = 1;
				II.inverted_index_compressed.push_back(entry);
				II.prev_doc_ids.push_back(doc_id);

				++unique_terms_found;
			}
			else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					// II.accumulator[it->second].push_back(doc_id);

					bool same = compress_uint64_differential_single(
							II.inverted_index_compressed[it->second].doc_ids, 
							doc_id,
							II.prev_doc_ids[it->second]
							);
					II.prev_doc_ids[it->second] = doc_id;


					if (same) {
						// ++II.inverted_index_compressed[it->second].term_freqs.back();
						++tf;
					}
					else {
						// II.inverted_index_compressed[it->second].term_freqs.push_back(1);
						add_rle_element_u8(II.inverted_index_compressed[it->second].term_freqs, tf);
						tf = 1;
					}
				}
			}
		}
		++doc_size;
	}
	doc_sizes.push_back(doc_size);
}
*/


void _BM25::process_doc_partition(
		const char* doc,
		const char terminator,
		uint64_t doc_id,
		uint64_t& unique_terms_found,
		uint16_t partition_id
		) {
	BM25Partition& IP = index_partitions[partition_id];

	uint64_t char_idx = 0;

	std::string term = "";
	// term.reserve(22);

	robin_hood::unordered_flat_set<uint64_t> terms_seen;

	// Split by commas not inside double quotes
	uint64_t doc_size = 0;
	uint8_t tf = 1;
	while (doc[char_idx] != terminator) {
		if (char_idx > 1048576) {
			std::cout << "Search field not found on line: " << doc_id << std::endl;
			std::cout << "Doc: " << doc << std::endl;
			std::cout << std::flush;
			std::exit(1);
		}
		if (doc[char_idx] == '\\') {
			++char_idx;
			term += toupper(doc[char_idx]);
			++char_idx;
			continue;
		}

		if (doc[char_idx] == ' ' && term == "") {
			++char_idx;
			continue;
		}

		if (doc[char_idx] == ' ') {
			if (stop_words.find(term) != stop_words.end()) {
				term.clear();
				++char_idx;
				++doc_size;
				continue;
			}

			auto [it, add] = IP.unique_term_mapping.try_emplace(term, unique_terms_found);
			if (add) {
				// New term
				// II.accumulator.push_back(entry);
				// convert uint64_t bytes to uint8_t bytes
				
				InvertedIndexElement entry;

				compress_uint64_differential_single(entry.doc_ids, doc_id, 0);
				entry.term_freqs.push_back(init_rle_element_u8(tf));

				IP.II.inverted_index_compressed.push_back(entry);
				IP.II.prev_doc_ids.push_back(doc_id);

				terms_seen.insert(it->second);

				++unique_terms_found;
			}
			else {
				if (check_rle_u8_row_size(IP.II.inverted_index_compressed[it->second].term_freqs, max_df)) {
					if (IP.II.inverted_index_compressed[it->second].doc_ids.size() >= 0) {
						IP.II.inverted_index_compressed[it->second].doc_ids.clear();
						IP.II.inverted_index_compressed[it->second].doc_ids.shrink_to_fit();
					}

					// Skip term
					term.clear();
					++char_idx;
					++doc_size;
					continue;
				}

				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert(it->second);

					bool same = compress_uint64_differential_single(
							IP.II.inverted_index_compressed[it->second].doc_ids, 
							doc_id,
							IP.II.prev_doc_ids[it->second]
							);
					IP.II.prev_doc_ids[it->second] = doc_id;

					if (same) {
						++tf;
					}
					else {
						add_rle_element_u8(IP.II.inverted_index_compressed[it->second].term_freqs, tf);
						tf = 1;
					}
				}
			}

			++doc_size;
			term.clear();

			++char_idx;
			continue;
		}

		term += toupper(doc[char_idx]);
		++char_idx;
	}

	if (term != "") {
		if (stop_words.find(term) == stop_words.end()) {
			auto [it, add] = IP.unique_term_mapping.try_emplace(term, unique_terms_found);

			if (add) {
				// New term
				// II.accumulator.push_back(entry);
				// convert uint64_t bytes to uint8_t bytes
				InvertedIndexElement entry;

				compress_uint64_differential_single(entry.doc_ids, doc_id, 0);
				entry.term_freqs.push_back(init_rle_element_u8(tf));
				tf = 1;
				IP.II.inverted_index_compressed.push_back(entry);
				IP.II.prev_doc_ids.push_back(doc_id);

				++unique_terms_found;
			}
			else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {

					bool same = compress_uint64_differential_single(
							IP.II.inverted_index_compressed[it->second].doc_ids, 
							doc_id,
							IP.II.prev_doc_ids[it->second]
							);
					IP.II.prev_doc_ids[it->second] = doc_id;


					if (same) {
						++tf;
					}
					else {
						add_rle_element_u8(IP.II.inverted_index_compressed[it->second].term_freqs, tf);
						tf = 1;
					}
				}
			}
		}
		++doc_size;
	}

	IP.doc_sizes.push_back(doc_size);
}

void _BM25::update_progress(int line_num, int num_lines, uint16_t partition_id) {
    const int bar_width = 121;

    float percentage = static_cast<float>(line_num) / num_lines;
    int pos = bar_width * percentage;

    std::string bar;
    if (pos == bar_width) {
        bar = "[" + std::string(bar_width - 1, '=') + ">" + "]";
    } else {
        bar = "[" + std::string(pos, '=') + ">" + std::string(bar_width - pos - 1, ' ') + "]";
    }

    std::string info = std::to_string(static_cast<int>(percentage * 100)) + "% " +
                       std::to_string(line_num) + " / " + std::to_string(num_lines) + " docs read";
    std::string output = "Partition " + std::to_string(partition_id + 1) + ": " + bar + " " + info;

    {
        std::lock_guard<std::mutex> lock(progress_mutex);

        progress_bars.resize(std::max(progress_bars.size(), static_cast<size_t>(partition_id + 1)));
        progress_bars[partition_id] = output;

        std::cout << "\033[s";  // Save the cursor position

		// Move the cursor to the appropriate position for this partition
        std::cout << "\033[" << (partition_id + 1 + init_cursor_row) << ";1H";

        std::cout << output << std::endl;

        std::cout << "\033[u";  // Restore the cursor to the original position after updating
        std::cout << std::flush;
    }
}

void _BM25::finalize_progress_bar() {
    std::cout << "\033[" << (num_partitions + 1 + init_cursor_row) << ";1H";
	fflush(stdout);
}


std::vector<uint64_t> get_II_row(
		InvertedIndex* II, 
		uint64_t term_idx
		) {
	std::vector<uint64_t> results_vector(
			1, 
			get_rle_u8_row_size(II->inverted_index_compressed[term_idx].term_freqs)
			);

	decompress_uint64(
			II->inverted_index_compressed[term_idx].doc_ids,
			results_vector
			);

	// Convert doc_ids back to absolute values
	for (size_t i = 2; i < results_vector.size(); ++i) {
		results_vector[i] += results_vector[i - 1];
	}

	// Get term frequencies
	for (size_t i = 0; i < II->inverted_index_compressed[term_idx].term_freqs.size(); ++i) {
		for (size_t j = 0; j < get_rle_element_u8_size(II->inverted_index_compressed[term_idx].term_freqs[i]); ++j) {
			results_vector.push_back(II->inverted_index_compressed[term_idx].term_freqs[i].value);
		}
	}
		

	return results_vector;
}


void _BM25::read_json(uint64_t start_byte, uint64_t end_byte, uint16_t partition_id) {
	FILE* f = reference_file_handles[partition_id];
	BM25Partition& IP = index_partitions[partition_id];

	// Quickly count number of lines in file
	uint64_t num_lines = 0;
	uint64_t total_bytes_read = 0;
	char buf[1024 * 1024];
	fseek(f, start_byte, SEEK_SET);
	while (size_t bytes_read = fread(buf, 1, sizeof(buf), f)) {
		for (size_t i = 0; i < bytes_read; ++i) {
			if (buf[i] == '\n') {
				++num_lines;
			}
			if (total_bytes_read++ >= end_byte) {
				break;
			}
		}
	}
	IP.num_docs = num_lines;

	// Reset file pointer to beginning
	fseek(f, start_byte, SEEK_SET);

	// Read the file line by line
	char*    line = NULL;
	size_t   len = 0;
	ssize_t  read;
	uint64_t line_num = 0;
	uint64_t byte_offset = start_byte;

	uint64_t unique_terms_found = 0;

	const int UPDATE_INTERVAL = 10000;
	while ((read = getline(&line, &len, f)) != -1) {

		if (byte_offset >= end_byte) {
			break;
		}

		if (!DEBUG) {
			if (line_num % UPDATE_INTERVAL == 0) {
				update_progress(line_num, num_lines, partition_id);
			}
		}
		if (strlen(line) == 0) {
			std::cout << "Empty line found" << std::endl;
			std::exit(1);
		}

		IP.line_offsets.push_back(byte_offset);
		byte_offset += read;

		// Iterate of line chars until we get to relevant column.

		// First char is always `{`
		int char_idx = 1;
		while (true) {
			start:
				while (line[char_idx] == ' ') {
					++char_idx;
				}

				// Found key. Match against search_col.
				if (line[char_idx] == '"') {
					// Iter over quote.
					++char_idx;

					for (const char& c : search_col) {
						// Scan until next key
						if (c != line[char_idx]) {
							while (line[char_idx] != ':') {
								if (line[char_idx] == '\\') {
									char_idx += 2;
									continue;
								}
								++char_idx;
							}
							// End of key

							// Scan until comma not in quotes
							while (line[char_idx] != ',') {
								if (line[char_idx] == '\\') {
									char_idx += 2;
									continue;
								}

								if (line[char_idx] == '"') {
									// Scan to next quote
									while (line[char_idx] != '"') {
										if (line[char_idx] == '\\') {
											char_idx += 2;
											continue;
										}
										++char_idx;
									}
								}
								++char_idx;
							}
							++char_idx;
							goto start;
						}
						++char_idx;
					}

					// Found key. 
					while (line[char_idx] != ':') {
						++char_idx;
					}
					++char_idx;

					// Go to first char of value.
					while (line[char_idx] == '"' || line[char_idx] == ' ') {
						++char_idx;
					}
					break;
				}
				else if (line[char_idx] == '}') {
					std::cout << "Search field not found on line: " << line_num << std::endl;
					std::cout << std::flush;
					std::exit(1);
				}
				else if (char_idx > 1048576) {
					std::cout << "Search field not found on line: " << line_num << std::endl;
					std::cout << std::flush;
					std::exit(1);
				}
				else {
					std::cerr << "Invalid json." << std::endl;
					std::cout << line << std::endl;
					std::cout << line[char_idx] << std::endl;
					std::cout << char_idx << std::endl;
					std::cout << std::flush;
					std::exit(1);
				}
		}

		process_doc_partition(&line[char_idx], '"', line_num, unique_terms_found, partition_id);
		++line_num;
	}
	update_progress(line_num, num_lines, partition_id);
	free(line);

	if (DEBUG) {
		std::cout << "Vocab size: " << unique_terms_found << std::endl;
	}

	IP.num_docs = num_lines;

	// Calc avg_doc_size
	double avg_doc_size = 0;
	for (const auto& size : IP.doc_sizes) {
		avg_doc_size += (double)size;
	}
	IP.avg_doc_size = (float)(avg_doc_size / num_lines);

	IP.II.prev_doc_ids.clear();
	IP.II.prev_doc_ids.shrink_to_fit();

	for (auto& row : IP.II.inverted_index_compressed) {
		if (row.doc_ids.size() == 0 || get_rle_u8_row_size(row.term_freqs) < (uint64_t)min_df) {
			row.doc_ids.clear();
			row.doc_ids.clear();
			row.term_freqs.clear();
			row.term_freqs.shrink_to_fit();
		}
	}


	if (DEBUG) {
		uint64_t total_size = 0;
		for (const auto& row : IP.II.inverted_index_compressed) {
			total_size += row.doc_ids.size();
			total_size += 3 * row.term_freqs.size();
		}
		total_size /= 1024 * 1024;
		std::cout << "Total size of inverted index: " << total_size << "MB" << std::endl;
	}
}

void _BM25::read_csv(uint64_t start_byte, uint64_t end_byte, uint16_t partition_id) {
	FILE* f = reference_file_handles[partition_id];
	BM25Partition& IP = index_partitions[partition_id];

	// Quickly count number of lines in file
	uint64_t num_lines = 0;
	uint64_t total_bytes_read = 0;
	char buf[1024 * 1024];

	if (fseek(f, start_byte, SEEK_SET) != 0) {
		std::cerr << "Error seeking file." << std::endl;
		std::exit(1);
	}

	while (total_bytes_read < (end_byte - start_byte)) {
		size_t bytes_read = fread(buf, 1, sizeof(buf), f);
		if (bytes_read == 0) {
			break;
		}

		for (size_t i = 0; i < bytes_read; ++i) {
			if (buf[i] == '\n') {
				++num_lines;
			}
			if (++total_bytes_read >= (end_byte - start_byte)) {
				break;
			}
		}
	}

	IP.num_docs = num_lines;

	// Reset file pointer to beginning
	if (fseek(f, start_byte, SEEK_SET) != 0) {
		std::cerr << "Error seeking file." << std::endl;
		std::exit(1);
	}

	int search_column_index = -1;

	// Read the file line by line
	char*    line = NULL;
	size_t   len = 0;
	ssize_t  read;
	uint64_t line_num = 0;
	uint64_t byte_offset = start_byte;

	uint64_t unique_terms_found = 0;

	// Small string optimization limit on most platforms
	std::string doc = "";
	doc.reserve(22);

	const int UPDATE_INTERVAL = 10000;
	while ((read = getline(&line, &len, f)) != -1) {

		if (byte_offset >= end_byte) {
			break;
		}

		if (line_num % UPDATE_INTERVAL == 0) update_progress(line_num, num_lines, partition_id);

		IP.line_offsets.push_back(byte_offset);
		byte_offset += read;

		// Iterate of line chars until we get to relevant column.
		int char_idx = 0;
		int col_idx  = 0;
		while (col_idx != search_col_idx) {
			if (line[char_idx] == '"') {
				// Skip to next quote.
				++char_idx;
				while (line[char_idx] == '"') {
					++char_idx;
				}
			}

			if (line[char_idx] == ',') {
				++col_idx;
			}
			++char_idx;
		}

		// Split by commas not inside double quotes
		char end_delim = ',';
		if (search_column_index == (int)columns.size() - 1) {
			end_delim = '\n';
		}
		if (line[char_idx] == '"') {
			end_delim = '"';
			++char_idx;
		}

		process_doc_partition(
				&line[char_idx], 
				end_delim, 
				line_num, 
				unique_terms_found, 
				partition_id
				);
		++line_num;
	}
	update_progress(line_num + 1, num_lines, partition_id);

	if (DEBUG) {
		std::cout << "Vocab size: " << unique_terms_found << std::endl;
	}

	IP.II.prev_doc_ids.clear();
	IP.II.prev_doc_ids.shrink_to_fit();

	IP.num_docs = IP.doc_sizes.size();

	// Calc avg_doc_size
	double avg_doc_size = 0;
	for (const auto& size : IP.doc_sizes) {
		avg_doc_size += (double)size;
	}
	IP.avg_doc_size = (float)(avg_doc_size / num_lines);

	/*
	for (auto& row : IP.II.inverted_index_compressed) {
		if (row.doc_ids.size() == 0 || get_rle_u8_row_size(row.term_freqs) < (uint64_t)min_df) {
			row.doc_ids.clear();
			row.doc_ids.clear();
			row.term_freqs.clear();
			row.term_freqs.shrink_to_fit();
		}
	}
	*/

	if (DEBUG) {
		uint64_t total_size = 0;
		for (const auto& row : IP.II.inverted_index_compressed) {
			total_size += row.doc_ids.size();
			total_size += 3 * row.term_freqs.size();
		}
		total_size /= 1024 * 1024;
		std::cout << "Total size of inverted index: " << total_size << "MB" << std::endl;
	}
}

/*
void _BM25::read_csv_memmap() {
	int fd = open(filename.c_str(), O_RDONLY);
	if (fd == -1) {
		std::cerr << "Error opening file for reading." << std::endl;
		std::exit(1);
	}

	struct stat sb;
	if (fstat(fd, &sb) == -1) {
		std::cerr << "Error getting file size." << std::endl;
		close(fd);
		std::exit(1);
	}

	size_t file_size = sb.st_size;

	char* file_data = (char*)mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (file_data == MAP_FAILED) {
		std::cerr << "Error mapping file to memory." << std::endl;
		close(fd);
		std::exit(1);
	}

	close(fd);

	// Quickly count number of lines in file
	uint64_t num_lines = 0;
	for (size_t i = 0; i < file_size; ++i) {
		if (file_data[i] == '\\') {
			i += 2;
		}
		if (file_data[i] == '"') {
			// Skip to next quote.
			while (file_data[i] != '"') ++i;
			++i;
		}
		if (file_data[i] == '\n') {
			++num_lines;
		}
	}

	num_docs = num_lines;
	if (max_df < 2.0f) {
		this->max_df = (int)num_docs * max_df;
	}
	printf("MAX DF: %d\n", (int)this->max_df);

	int search_column_index = -1;

	// Read the file line by line
	char*    line = NULL;
	uint64_t line_num = 0;
	uint64_t byte_offset = 0;

	// Get col names
	line = file_data;
	std::istringstream iss(line);
	std::string value;
	while (*line != '\n' && *line != '\0') {
		if (*line == ',') {
            columns.push_back(value);
            if (value == search_col) {
                search_column_index = columns.size() - 1;
            }
            value.clear();
        } else {
            value.push_back(*line);
        }
        ++line;
    }

    if (!value.empty()) {
        columns.push_back(value);
        if (value == search_col) {
            search_column_index = columns.size() - 1;
        }
    }

	if (search_column_index == -1) {
		std::cerr << "Search column not found in header" << std::endl;
		std::cerr << "Cols found:  ";
		for (size_t i = 0; i < columns.size(); ++i) {
			std::cerr << columns[i] << ",";
		}
		std::cerr << std::endl;
		exit(1);
	}

	++line;
	byte_offset = line - file_data;

	uint64_t unique_terms_found = 0;

	// Small string optimization limit on most platforms
	std::string doc = "";
	doc.reserve(22);

	// robin_hood::unordered_flat_set<uint64_t> terms_seen;

	const int UPDATE_INTERVAL = 10000;
	// while ((read = getline(&line, &len, reference_file)) != -1) {
	while (byte_offset < file_size) {
		line = file_data + byte_offset;

		if (line_num % UPDATE_INTERVAL == 0) {
			update_progress(line_num, num_lines);
		}
		line_offsets.push_back(byte_offset);

		// Iterate of line chars until we get to relevant column.
		int char_idx = 0;
		int col_idx  = 0;
		while (col_idx != search_column_index) {
			if (line[char_idx] == '"') {
				// Skip to next quote.
				++char_idx;
				while (line[char_idx] == '"') {
					++char_idx;
				}
			}

			if (line[char_idx] == ',') {
				++col_idx;
			}
			++char_idx;
		}

		// Split by commas not inside double quotes
		char end_delim = ',';
		if (search_column_index == (int)columns.size() - 1) {
			end_delim = '\n';
		}
		if (line[char_idx] == '"') {
			end_delim = '"';
			++char_idx;
		}

		process_doc(&line[char_idx], end_delim, line_num, unique_terms_found);
		++line_num;
		byte_offset += strlen(line) + 1;
	}
	update_progress(line_num + 1, num_lines);

	munmap(file_data, file_size);

	if (DEBUG) {
		std::cout << "Vocab size: " << unique_terms_found << std::endl;
	}

	II.prev_doc_ids.clear();
	II.prev_doc_ids.shrink_to_fit();

	num_docs = doc_sizes.size();

	// Calc avg_doc_size
	float sum = 0;
	for (const auto& size : doc_sizes) {
		sum += size;
	}
	avg_doc_size = sum / num_docs;

	if (DEBUG) {
		uint64_t total_size = 0;
		for (const auto& row : II.inverted_index_compressed) {
			total_size += row.doc_ids.size();
			total_size += 3 * row.term_freqs.size();
		}
		total_size /= 1024 * 1024;
		std::cout << "Total size of inverted index: " << total_size << "MB" << std::endl;
	}
}
*/


std::vector<std::pair<std::string, std::string>> _BM25::get_csv_line(int line_num, uint16_t partition_id) {
	FILE* f = reference_file_handles[partition_id];
	BM25Partition& IP = index_partitions[partition_id];

	// seek from FILE* reference_file which is already open
	fseek(f, IP.line_offsets[line_num], SEEK_SET);
	char* line = NULL;
	size_t len = 0;
	ssize_t read = getline(&line, &len, f);

	// Create effective json by combining column names with values split by commas
	std::vector<std::pair<std::string, std::string>> row;
	std::string cell;
	bool in_quotes = false;
	size_t col_idx = 0;

	for (size_t i = 0; i < (size_t)read - 1; ++i) {
		if (line[i] == '"') {
			in_quotes = !in_quotes;
		}
		else if (line[i] == ',' && !in_quotes) {
			row.emplace_back(columns[col_idx], cell);
			cell.clear();
			++col_idx;
		}
		else {
			cell += line[i];
		}
	}
	row.emplace_back(columns[col_idx], cell);
	return row;
}


std::vector<std::pair<std::string, std::string>> _BM25::get_json_line(int line_num, uint16_t partition_id) {
	FILE* f = reference_file_handles[partition_id];
	BM25Partition& IP = index_partitions[partition_id];

	// seek from FILE* reference_file which is already open
	fseek(f, IP.line_offsets[line_num], SEEK_SET);
	char* line = NULL;
	size_t len = 0;
	getline(&line, &len, f);

	// Create effective json by combining column names with values split by commas
	std::vector<std::pair<std::string, std::string>> row;

	std::string first  = "";
	std::string second = "";

	if (line[1] == '}') {
		return row;
	}

	size_t char_idx = 2;
	while (true) {
		while (line[char_idx] != '"') {
			if (line[char_idx] == '\\') {
				++char_idx;
				first += line[char_idx];
				++char_idx;
				continue;
			}
			first += line[char_idx];
			++char_idx;
		}
		char_idx += 2;

		// Go to first char of value.
		while (line[char_idx] == '"' || line[char_idx] == ' ') {
			++char_idx;
		}

		while (line[char_idx] != '}' || line[char_idx] != '"' || line[char_idx] != ',') {
			if (line[char_idx] == '\\') {
				++char_idx;
				second += line[char_idx];
				++char_idx;
				continue;
			}
			else if (line[char_idx] == '}') {
				second += line[char_idx];
				row.emplace_back(first, second);
				return row;
			}
			second += line[char_idx];
			++char_idx;
		}
		++char_idx;
		if (line[char_idx] == '}') {
			return row;
		}
	}
	return row;
}


void _BM25::save_to_disk(const std::string& db_dir) {
	auto start = std::chrono::high_resolution_clock::now();

	if (access(db_dir.c_str(), F_OK) != -1) {
		// Remove the directory if it exists
		std::string command = "rm -r " + db_dir;
		system(command.c_str());

		// Create the directory
		command = "mkdir " + db_dir;
		system(command.c_str());
	}
	else {
		// Create the directory if it does not exist
		std::string command = "mkdir " + db_dir;
		system(command.c_str());
	}

	// Join paths
	std::string UNIQUE_TERM_MAPPING_PATH = db_dir + "/unique_term_mapping.bin";
	std::string INVERTED_INDEX_PATH 	 = db_dir + "/inverted_index.bin";
	std::string DOC_SIZES_PATH 		     = db_dir + "/doc_sizes.bin";
	std::string LINE_OFFSETS_PATH 		 = db_dir + "/line_offsets.bin";
	std::string METADATA_PATH 			 = db_dir + "/metadata.bin";

	for (uint16_t partition_id = 0; partition_id < num_partitions; ++partition_id) {
		BM25Partition& IP = index_partitions[partition_id];

		serialize_robin_hood_flat_map_string_u64(IP.unique_term_mapping, UNIQUE_TERM_MAPPING_PATH + "_" + std::to_string(partition_id));
		serialize_inverted_index(IP.II, INVERTED_INDEX_PATH + "_" + std::to_string(partition_id));

		std::vector<uint8_t> compressed_doc_sizes;
		std::vector<uint8_t> compressed_line_offsets;
		compressed_doc_sizes.reserve(IP.doc_sizes.size() * 2);
		compressed_line_offsets.reserve(IP.line_offsets.size() * 2);
		compress_uint64(IP.doc_sizes, compressed_doc_sizes);
		compress_uint64(IP.line_offsets, compressed_line_offsets);

		serialize_vector_u8(compressed_doc_sizes, DOC_SIZES_PATH + "_" + std::to_string(partition_id));
		serialize_vector_u8(compressed_line_offsets, LINE_OFFSETS_PATH + "_" + std::to_string(partition_id));
	}

	// Serialize smaller members.
	std::ofstream out_file(METADATA_PATH, std::ios::binary);
	if (!out_file) {
		std::cerr << "Error opening file for writing.\n";
		return;
	}

	// Write basic types directly
	out_file.write(reinterpret_cast<const char*>(&num_docs), sizeof(num_docs));
	out_file.write(reinterpret_cast<const char*>(&min_df), sizeof(min_df));
	out_file.write(reinterpret_cast<const char*>(&max_df), sizeof(max_df));
	out_file.write(reinterpret_cast<const char*>(&k1), sizeof(k1));
	out_file.write(reinterpret_cast<const char*>(&b), sizeof(b));

	for (uint16_t partition_id = 0; partition_id < num_partitions; ++partition_id) {
		BM25Partition& IP = index_partitions[partition_id];

		out_file.write(reinterpret_cast<const char*>(&IP.avg_doc_size), sizeof(IP.avg_doc_size));
	}

	// Write enum as int
	int file_type_int = static_cast<int>(file_type);
	out_file.write(reinterpret_cast<const char*>(&file_type_int), sizeof(file_type_int));

	// Write std::string
	size_t filename_length = filename.size();
	out_file.write(reinterpret_cast<const char*>(&filename_length), sizeof(filename_length));
	out_file.write(filename.data(), filename_length);

	// Write std::vector<std::string>
	size_t columns_size = columns.size();
	out_file.write(reinterpret_cast<const char*>(&columns_size), sizeof(columns_size));
	for (const auto& col : columns) {
		size_t col_length = col.size();
		out_file.write(reinterpret_cast<const char*>(&col_length), sizeof(col_length));
		out_file.write(col.data(), col_length);
	}

	// Write search_col std::string
	size_t search_col_length = search_col.size();
	out_file.write(reinterpret_cast<const char*>(&search_col_length), sizeof(search_col_length));
	out_file.write(search_col.data(), search_col_length);

	out_file.close();

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsed_seconds = end - start;

	if (DEBUG) {
		std::cout << "Saved in " << elapsed_seconds.count() << "s" << std::endl;
	}
}

void _BM25::load_from_disk(const std::string& db_dir) {
	auto start = std::chrono::high_resolution_clock::now();

	// Join paths
	std::string UNIQUE_TERM_MAPPING_PATH = db_dir + "/unique_term_mapping.bin";
	std::string INVERTED_INDEX_PATH 	 = db_dir + "/inverted_index.bin";
	std::string DOC_SIZES_PATH 		     = db_dir + "/doc_sizes.bin";
	std::string LINE_OFFSETS_PATH 		 = db_dir + "/line_offsets.bin";
	std::string METADATA_PATH 			 = db_dir + "/metadata.bin";

	for (uint16_t partition_id = 0; partition_id < num_partitions; ++partition_id) {
		BM25Partition& IP = index_partitions[partition_id];

		deserialize_robin_hood_flat_map_string_u64(IP.unique_term_mapping, UNIQUE_TERM_MAPPING_PATH + "_" + std::to_string(partition_id));
		deserialize_inverted_index(IP.II, INVERTED_INDEX_PATH + "_" + std::to_string(partition_id));

		std::vector<uint8_t> compressed_doc_sizes;
		std::vector<uint8_t> compressed_line_offsets;
		deserialize_vector_u8(compressed_doc_sizes, DOC_SIZES_PATH + "_" + std::to_string(partition_id));
		deserialize_vector_u8(compressed_line_offsets, LINE_OFFSETS_PATH + "_" + std::to_string(partition_id));

		decompress_uint64(compressed_doc_sizes, IP.doc_sizes);
		decompress_uint64(compressed_line_offsets, IP.line_offsets);
	}

	// Load smaller members.
	std::ifstream in_file(METADATA_PATH, std::ios::binary);
    if (!in_file) {
        std::cerr << "Error opening file for reading.\n";
        return;
    }

    // Read basic types directly
    in_file.read(reinterpret_cast<char*>(&num_docs), sizeof(num_docs));
    in_file.read(reinterpret_cast<char*>(&min_df), sizeof(min_df));
    in_file.read(reinterpret_cast<char*>(&max_df), sizeof(max_df));
    in_file.read(reinterpret_cast<char*>(&k1), sizeof(k1));
    in_file.read(reinterpret_cast<char*>(&b), sizeof(b));

	for (uint16_t partition_id = 0; partition_id < num_partitions; ++partition_id) {
		BM25Partition& IP = index_partitions[partition_id];

		in_file.read(reinterpret_cast<char*>(&IP.avg_doc_size), sizeof(IP.avg_doc_size));
	}

    // Read enum as int
    int file_type_int;
    in_file.read(reinterpret_cast<char*>(&file_type_int), sizeof(file_type_int));
    file_type = static_cast<SupportedFileTypes>(file_type_int);

    // Read std::string
    size_t filename_length;
    in_file.read(reinterpret_cast<char*>(&filename_length), sizeof(filename_length));
    filename.resize(filename_length);
    in_file.read(&filename[0], filename_length);

    // Read std::vector<std::string>
    size_t columns_size;
    in_file.read(reinterpret_cast<char*>(&columns_size), sizeof(columns_size));
    columns.resize(columns_size);
    for (auto& col : columns) {
        size_t col_length;
        in_file.read(reinterpret_cast<char*>(&col_length), sizeof(col_length));
        col.resize(col_length);
        in_file.read(&col[0], col_length);
    }

    // Read search_col std::string
    size_t search_col_length;
    in_file.read(reinterpret_cast<char*>(&search_col_length), sizeof(search_col_length));
    search_col.resize(search_col_length);
    in_file.read(&search_col[0], search_col_length);

    in_file.close();

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsed_seconds = end - start;

	if (DEBUG) {
		std::cout << "Loaded in " << elapsed_seconds.count() << "s" << std::endl;
	}
}


_BM25::_BM25(
		std::string filename,
		std::string search_col,
		int min_df,
		float max_df,
		float k1,
		float b,
		uint16_t num_partitions,
		const std::vector<std::string>& _stop_words
		) : min_df(min_df), 
			max_df(max_df), 
			k1(k1), 
			b(b),
			num_partitions(num_partitions),
			search_col(search_col), 
			filename(filename) {

	progress_bars.resize(num_partitions);

	for (const std::string& stop_word : _stop_words) {
		stop_words.insert(stop_word);
	}

	// Open file handles
	for (uint16_t i = 0; i < num_partitions; ++i) {
		FILE* f = fopen(filename.c_str(), "r");
		if (f == NULL) {
			std::cerr << "Unable to open file: " << filename << std::endl;
			exit(1);
		}
		reference_file_handles.push_back(f);
	}

	auto overall_start = std::chrono::high_resolution_clock::now();

	std::vector<uint64_t> partition_boundaries;
	std::vector<std::thread> threads;

	index_partitions.resize(num_partitions);
	num_docs = 0;

	int col;
	get_cursor_position(init_cursor_row, col);
	get_terminal_size(terminal_height, col);

	if (terminal_height - init_cursor_row < num_partitions + 1) {
		// Perform neccessary scroll
		std::cout << "\x1b[" << num_partitions + 1 << "S";
		init_cursor_row -= num_partitions + 1;
	}

	// Read file to get documents, line offsets, and columns
	if (filename.substr(filename.size() - 3, 3) == "csv") {

		proccess_csv_header();
		determine_partition_boundaries_csv(partition_boundaries);

		// Launch num_partitions threads to read csv file
		for (uint16_t i = 0; i < num_partitions; ++i) {
			threads.push_back(std::thread(
				[this, &partition_boundaries, i] {
					read_csv(partition_boundaries[i], partition_boundaries[i + 1], i);
				}
			));
		}

		file_type = CSV;
	}
	else if (filename.substr(filename.size() - 4, 4) == "json") {
		determine_partition_boundaries_json(partition_boundaries);

		// Launch num_partitions threads to read json file
		for (uint16_t i = 0; i < num_partitions; ++i) {
			threads.push_back(std::thread(
				[this, &partition_boundaries, i] {
					read_json(partition_boundaries[i], partition_boundaries[i + 1], i);
				}
			));
		}
		num_docs = index_partitions[0].num_docs;
		for (uint16_t i = 1; i < num_partitions; ++i) {
			num_docs += index_partitions[i].num_docs;
		}
		if (max_df < 2.0f) {
			this->max_df = (int)num_docs * max_df;
		}

		file_type = JSON;
	}
	else {
		std::cout << "Only csv and json files are supported." << std::endl;
		std::exit(1);
	}

	for (auto& thread : threads) {
		thread.join();
	}

	finalize_progress_bar();

	if (DEBUG) {
		auto read_end = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double> read_elapsed_seconds = read_end - overall_start;
		std::cout << "Read file in " << read_elapsed_seconds.count() << " seconds" << std::endl;
	}
}


_BM25::_BM25(
		std::vector<std::string>& documents,
		int min_df,
		float max_df,
		float k1,
		float b,
		uint16_t num_partitions,
		const std::vector<std::string>& _stop_words
		) : min_df(min_df), 
			max_df(max_df), 
			k1(k1), 
			b(b),
			num_partitions(num_partitions) {
	
	for (const std::string& stop_word : _stop_words) {
		stop_words.insert(stop_word);
	}

	filename = "in_memory";
	file_type = IN_MEMORY;

	num_docs = documents.size();

	if (max_df < 2.0f) {
		this->max_df = (int)num_docs * max_df;
	}

	uint64_t unique_terms_found = 0;

	std::string doc = "";
	doc.reserve(22);

	index_partitions.resize(num_partitions);

	std::vector<uint64_t> partition_boundaries(num_partitions + 1);
	for (uint16_t i = 0; i < num_partitions; ++i) {
		partition_boundaries[i] = (uint64_t)i * (num_docs / num_partitions);
	}
	partition_boundaries[num_partitions] = num_docs;

	// const int UPDATE_INTERVAL = 10000;

	// Do on all partition w/ thread lib
	std::vector<std::thread> threads;
	for (uint16_t i = 0; i < num_partitions; ++i) {
		threads.push_back(std::thread(
			[&documents, &partition_boundaries, i, this] {
				uint64_t doc_id = partition_boundaries[i];
				uint64_t end = partition_boundaries[i + 1];
				uint64_t unique_terms_found = 0;
				std::string doc = "";
				doc.reserve(22);
				for (; doc_id < end; ++doc_id) {
					process_doc_partition(documents[doc_id].c_str(), '\0', doc_id, unique_terms_found, i);
				}
			}
		));
	}

	if (DEBUG) {
		std::cout << "Vocab size: " << unique_terms_found << std::endl;
	}

	for (auto& thread : threads) {
		thread.join();
	}

	for (uint16_t i = 0; i < num_partitions; ++i) {
		BM25Partition& IP = index_partitions[i];

		IP.II.prev_doc_ids.clear();
		IP.II.prev_doc_ids.shrink_to_fit();

		for (auto& row : IP.II.inverted_index_compressed) {
			if (row.doc_ids.size() == 0 || get_rle_u8_row_size(row.term_freqs) < (uint64_t)min_df) {
				row.doc_ids.clear();
				row.doc_ids.clear();
				row.term_freqs.clear();
				row.term_freqs.shrink_to_fit();
			}
		}
	}

	if (DEBUG) {
		uint64_t total_size = 0;

		for (uint16_t i = 0; i < num_partitions; ++i) {
			BM25Partition& IP = index_partitions[i];
			for (const auto& row : IP.II.inverted_index_compressed) {
				total_size += row.doc_ids.size();
				total_size += 3 * row.term_freqs.size();
			}
		}
		total_size /= 1024 * 1024;
		std::cout << "Total size of inverted index: " << total_size << "MB" << std::endl;
	}
}

inline float _BM25::_compute_bm25(
		uint64_t doc_id,
		float tf,
		float idf,
		uint16_t partition_id
		) {
	BM25Partition& IP = index_partitions[partition_id];

	float doc_size = IP.doc_sizes[doc_id];

	return idf * tf / (tf + k1 * (1 - b + b * doc_size / IP.avg_doc_size));
}

std::vector<BM25Result> _BM25::_query_partition(
		std::string& query, 
		uint32_t k,
		uint32_t init_max_df,
		uint16_t partition_id
		) {
	auto start = std::chrono::high_resolution_clock::now();
	std::vector<uint64_t> term_idxs;
	BM25Partition& IP = index_partitions[partition_id];

	std::string substr = "";
	for (const char& c : query) {
		if (c != ' ') {
			substr += toupper(c); 
			continue;
		}
		if (IP.unique_term_mapping.find(substr) == IP.unique_term_mapping.end()) {
			continue;
		}

		if (IP.II.inverted_index_compressed[IP.unique_term_mapping[substr]].doc_ids.size() < (uint64_t)min_df) {
			substr.clear();
			continue;
		}

		term_idxs.push_back(IP.unique_term_mapping[substr]);
		substr.clear();
	}

	if (IP.unique_term_mapping.find(substr) != IP.unique_term_mapping.end()) {
		if (IP.II.inverted_index_compressed[IP.unique_term_mapping[substr]].doc_ids.size() > 0) {
			term_idxs.push_back(IP.unique_term_mapping[substr]);
		}
		else {
			printf("Term %s has too few docs\n", substr.c_str());
		}

	}

	if (term_idxs.size() == 0) {
		return std::vector<BM25Result>();
	}

	// Gather docs that contain at least one term from the query
	// Uses dynamic max_df for performance
	uint64_t local_max_df = std::min((uint64_t)init_max_df, (uint64_t)max_df);
	robin_hood::unordered_map<uint64_t, float> doc_scores;

	for (const uint64_t& term_idx : term_idxs) {
		uint64_t df;
		vbyte_decode_uint64(
				IP.II.inverted_index_compressed[term_idx].doc_ids.data(),
				&df
				);

		// if (df > local_max_df) continue;

		float idf = log((num_docs - df + 0.5) / (df + 0.5));

		std::vector<uint64_t> results_vector = get_II_row(&IP.II, term_idx);
		uint64_t num_matches = (results_vector.size() - 1) / 2;

		if (DEBUG) {
			std::cout << "DF: " << df << std::endl;
			std::cout << "LOCAL MAX DF: " << local_max_df << std::endl;
			std::cout << "AVG DOC SIZE: " << IP.avg_doc_size << std::endl;
			std::cout << "NUM DOCS: " << IP.num_docs << std::endl;
			std::cout << "NUM MATCHES: " << num_matches << std::endl;
			std::cout << "K: " << k << std::endl;
		}

		if (num_matches == 0) {
			continue;
		}

		for (size_t i = 0; i < num_matches; ++i) {
			uint64_t doc_id  = results_vector[i + 1];
			float tf 		 = (float)results_vector[i + num_matches + 1];
			float bm25_score = _compute_bm25(doc_id, tf, idf, partition_id);

			if (doc_scores.find(doc_id) == doc_scores.end()) {
				doc_scores[doc_id] = bm25_score;
			}
			else {
				doc_scores[doc_id] += bm25_score;
			}
		}
	}
	
	if (doc_scores.size() == 0) {
		return std::vector<BM25Result>();
	}

	start = std::chrono::high_resolution_clock::now();
	std::priority_queue<
		BM25Result,
		std::vector<BM25Result>,
		_compare_bm25_result> top_k_docs;

	for (const auto& pair : doc_scores) {
		BM25Result result {
			.doc_id = pair.first,
			.score  = pair.second,
			.partition_id = partition_id
		};
		top_k_docs.push(result);
		if (top_k_docs.size() > k) {
			top_k_docs.pop();
		}
	}

	std::vector<BM25Result> result(top_k_docs.size());
	int idx = top_k_docs.size() - 1;
	while (!top_k_docs.empty()) {
		result[idx] = top_k_docs.top();
		top_k_docs.pop();
		--idx;
	}

	if (DEBUG) {
		auto end = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double, std::milli> elapsed_ms = end - start;
		std::cout << "QUERY: " << query << std::endl;
		std::cout << "Number of docs: " << doc_scores.size() << "    ";
		std::cout << elapsed_ms.count() << "ms" << std::endl;
		fflush(stdout);
	}

	return result;
}

std::vector<BM25Result> _BM25::query(
		std::string& query, 
		uint32_t k,
		uint32_t init_max_df
		) {
	auto start = std::chrono::high_resolution_clock::now();

	std::vector<std::thread> threads;
	std::vector<std::vector<BM25Result>> results(num_partitions);

	// _query_partition on each thread
	for (uint16_t i = 0; i < num_partitions; ++i) {
		threads.push_back(std::thread(
			[this, &query, k, init_max_df, i, &results] {
				results[i] = _query_partition(query, k, init_max_df, i);
			}
		));
	}

	for (auto& thread : threads) {
		thread.join();
	}

	if (results.size() == 0) {
		return std::vector<BM25Result>();
	}

	uint64_t total_matching_docs = 0;

	// Join results. Keep global max heap of size k
	std::priority_queue<
		BM25Result,
		std::vector<BM25Result>,
		_compare_bm25_result> top_k_docs;

	for (const auto& partition_results : results) {
		total_matching_docs += partition_results.size();
		for (const auto& pair : partition_results) {
			top_k_docs.push(pair);
			if (top_k_docs.size() > k) {
				top_k_docs.pop();
			}
		}
	}
	
	std::vector<BM25Result> result(top_k_docs.size());
	int idx = top_k_docs.size() - 1;
	while (!top_k_docs.empty()) {
		result[idx] = top_k_docs.top();
		top_k_docs.pop();
		--idx;
	}

	if (DEBUG) {
		auto end = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double, std::milli> elapsed_ms = end - start;
		std::cout << "QUERY: " << query << std::endl;
		std::cout << "Total matching docs: " << total_matching_docs << "    ";
		std::cout << elapsed_ms.count() << "ms" << std::endl;
		fflush(stdout);
	}

	return result;
}

std::vector<std::vector<std::pair<std::string, std::string>>> _BM25::get_topk_internal(
		std::string& _query,
		uint32_t top_k,
		uint32_t init_max_df
		) {
	std::vector<std::vector<std::pair<std::string, std::string>>> result;
	std::vector<BM25Result> top_k_docs = query(_query, top_k, init_max_df);
	result.reserve(top_k_docs.size());

	std::vector<std::pair<std::string, std::string>> row;
	for (size_t i = 0; i < top_k_docs.size(); ++i) {
		switch (file_type) {
			case CSV:
				row = get_csv_line(top_k_docs[i].doc_id, top_k_docs[i].partition_id);
				break;
			case JSON:
				row = get_json_line(top_k_docs[i].doc_id, top_k_docs[i].partition_id);
				break;
			case IN_MEMORY:
				std::cout << "Error: In-memory data not supported for this function." << std::endl;
				std::exit(1);
				break;
			default:
				std::cout << "Error: Incorrect file type" << std::endl;
				std::exit(1);
				break;
		}
		row.push_back(std::make_pair("score", std::to_string(top_k_docs[i].score)));
		result.push_back(row);
	}
	return result;
}
