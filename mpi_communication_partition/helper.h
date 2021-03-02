#ifndef CATO_HELPER_H
#define CATO_HELPER_H

#include "debug.h"
#include "analysis_results.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <vector>

/**
 * Returns a vector of all Users of the named function
 * Can be used to find all instances where the named function is called in the
 * IR Module M
 **/
std::vector<llvm::User*> get_function_users(llvm::Module&, llvm::StringRef);

// get last instruction  in the sense that the return value post dominates all other instruction in the given list
// nullptr if not found
template<class T>
std::enable_if_t<std::is_base_of<llvm::Instruction, T>::value, T>* get_last_instruction(
		std::vector<T*> inst_list);

// same as get last instruction but for first instruction
template<class T>
std::enable_if_t<std::is_base_of<llvm::Instruction, T>::value, T>* get_first_instruction(
		std::vector<T*> inst_list);

// definition of these templates are at the end of this header

/**
 * Searches the Function for all uses of instructions of type T and returns them in a
 * vector
 **/
template<class T> std::vector<T*> get_instruction_in_function(
		llvm::Function *func) {
	std::vector<T*> instructions;
	for (auto &B : *func) {
		for (auto &I : B) {
			if (auto *inst = llvm::dyn_cast<T>(&I)) {
				instructions.push_back(inst);
			}
		}
	}
	return instructions;
}

template <class T>
void remove_eraze_nullptr(std::vector<T*> &remove_nulls){
	remove_nulls.erase(
			std::remove_if(remove_nulls.begin(),
					remove_nulls.end(), [](auto *v) {
						return v == nullptr;
					}),remove_nulls.end());
}

//combine std::find_if and std::none_of to write stl-like find_if_exactly_one
template<class InputIt, class UnaryPredicate>
InputIt find_if_exactly_one(InputIt first, InputIt last, UnaryPredicate p) {
	auto it = std::find_if(first, last, p);
	if ((it != last) && std::none_of(std::next(it), last, p))
		return it;
	else
		return last;
}

/**
 * Searches the whole Module for all instructions of Type T and returns them in a
 * vector
 **/
template<class T> std::vector<T*> get_instruction_in_module(llvm::Module &M) {
	std::vector<T*> instructions;
	for (auto &F : M) {
		for (auto &B : F) {
			for (auto &I : B) {
				if (auto *instruction = llvm::dyn_cast<T>(&I)) {
					instructions.push_back(instruction);
				}
			}
		}
	}
	return instructions;
}

/**
 * Returns the length of the pointer chain of a given value
 * For example the base pointer to a 2D array (int** arr) would return 2
 **/
int get_pointer_depth(llvm::Type*);
int get_pointer_depth(llvm::Value*);

/**
 * Returns the corresponding MPI_Datatype for a llvm Type
 **/
int get_mpi_datatype(llvm::Type*);
int get_mpi_datatype(llvm::Value*);

/**
 * Returns the size of the given Type in bytes
 **/
size_t get_size_in_Byte(llvm::Module&, llvm::Value*);
size_t get_size_in_Byte(llvm::Module&, llvm::Type*);

//definitions of templated functions:
// helper for template defnintions:
template<class T> inline std::enable_if_t<
		std::is_base_of<llvm::Instruction, T>::value, T>* get_first_instruction(
		T *A, T *B);

// True if A is proven before B
//  False if B is proven before A
// assert fail if no order between A and B can be proven
template<class T> inline std::enable_if_t<
		std::is_base_of<llvm::Instruction, T>::value, bool> is_instruction_before(
		T *A, T *B);

//explicit template specialization if one argument is instruction and the other is subclass
inline bool is_instruction_before(llvm::Instruction *A, llvm::Instruction *B) {
	return is_instruction_before<llvm::Instruction>(A, B);
}
// it does not "preserve" the old type of the instructions
inline llvm::Instruction* get_first_instruction(llvm::Instruction *A, llvm::Instruction *B) {
	return get_first_instruction<llvm::Instruction>(A, B);
}

template<class T>
std::enable_if_t<std::is_base_of<llvm::Instruction, T>::value, T>* get_first_instruction(
		std::vector<T*> inst_list) {

	T *current_instruction = inst_list[0];
	bool is_viable = true;

	if (inst_list.size() == 0) {
		return nullptr;
	}

	Debug(
			// no nullptrs are allowed
			for (auto* i : inst_list) {assert(i!=nullptr);}
	)

	// first entry is current candidate therefore begin at +1
	for (auto it = inst_list.begin() + 1; it < inst_list.end(); ++it) {
		auto *first_inst = get_first_instruction(current_instruction, *it);
		if (first_inst == nullptr) {
			// no instruction dominates the other
			is_viable = false;
		} else if (first_inst == *it) {
			current_instruction = *it;
		}
	}

	if (is_viable) {
		return current_instruction;
	} else {
		// need to double-check if this dominates all other instructions
		// Maybe A->C b-> C but the missing order between A and B will set is_viable flag to false
		// therefore we need to recheck
		for (auto *i : inst_list) {
			if (i != current_instruction
					&& current_instruction
							== get_first_instruction(current_instruction, i)) {
				// found a non dominate relation: abort

				llvm::errs() << "Error in finding the first instruction \n";

				return nullptr;
			}
		}
		// successfully passed test above
		return current_instruction;

	}

}

//TODO this function needs the PdomTree ananlyis. Maybe move it to analysis instead of helper funcitons?
// get last instruction  in the sense that the return value post dominates all other instruction in the given list
// nullptr if not found
template<class T>
std::enable_if_t<std::is_base_of<llvm::Instruction, T>::value, T>* get_last_instruction(
		std::vector<T*> inst_list) {

	if (inst_list.size() == 0) {
		return nullptr;
	}

	Debug(
			// no nullptrs are allowed
			for (auto* i : inst_list) {assert(i!=nullptr);}
	)

	T *current_instruction = inst_list[0];
	bool is_viable = true;

// first entry is current candidate therefore begin at +1
	for (auto it = inst_list.begin() + 1; it < inst_list.end(); ++it) {
		auto *first_inst = get_first_instruction(current_instruction, *it);
		if (first_inst == nullptr) {
			// no instruction dominates the other
			is_viable = false;
		} else if (first_inst == current_instruction) {
			current_instruction = *it;
		}
	}

	if (is_viable) {
		return current_instruction;
	} else {
		// need to double-check if this dominates all other instructions
		// Maybe A->C b-> C but the missing order between A and B will set is_viable flag to false
		// therefore we need to recheck
		for (auto *i : inst_list) {
			if (i != current_instruction
					&& current_instruction
							!= get_first_instruction(current_instruction, i)) {
				// found a non dominate relation: abort

				llvm::errs() << "Error in finding the first instruction \n";

				return nullptr;
			}
		}
		// successfully passed test above
		return current_instruction;

	}

}

//helper for templated functions above

// return A if A is proven before B
// return B if B is proven before A
// nullptr other wise
// A is considered before B if it either dominates B or is postdominated by B
// meaning that in every path if A and B are present A must come before B
template<class T> inline std::enable_if_t<
		std::is_base_of<llvm::Instruction, T>::value, T>* get_first_instruction(
		T *A, T *B) {
	auto *Pdomtree = analysis_results->getPostDomTree(A->getFunction());
	auto *Domtree = analysis_results->getDomTree(A->getFunction());

	if (Pdomtree->dominates(B, A) || Domtree->dominates(A, B)) {
		return A;
	} else if (Pdomtree->dominates(A, B) || Domtree->dominates(B, A)) {
		return B;
	} else {

		return nullptr;
	}
}

// True if A is proven before B
//  False if B is proven before A
// assert fail if no order between A and B can be proven
template<class T> inline std::enable_if_t<
		std::is_base_of<llvm::Instruction, T>::value, bool> is_instruction_before(
		T *A, T *B) {
	auto *first = get_first_instruction(A, B);
	if (first == A) {
		return true;
	} else if (first == B) {
		return false;
	} else {
Debug(
	auto *Pdomtree = analysis_results->getPostDomTree(A->getFunction());
	auto *Domtree = analysis_results->getDomTree(A->getFunction());
	llvm::errs() << "could not analyze relation between  A and B\n";
	A->dump();
	B->dump();

	llvm::errs() << "A dominates B ?" << Domtree->dominates(A, B) << "\n";
	llvm::errs() << "B dominates A ?" << Domtree->dominates(B, A) << "\n";
	llvm::errs() << "A Postdominates B ?" << Pdomtree->dominates(A, B) << "\n";
	llvm::errs() << "B Postdominates A ?" << Pdomtree->dominates(B, A) << "\n";

	llvm::errs() << "Dom Tree verify?" << Domtree->verify() << "\n";
	A->getFunction()->dump();
	)
																					assert(false);
		return false;
	}

}

#endif
