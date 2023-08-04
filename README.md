# Tree-Based File System

This repository contains a thread-safe, real tree-based (not inodes) file system implementation written in C.
It was developed as part of a course at University of Warsaw (MIMUW). What was difficult about it?
A method `tree_move` had to be implemented, which operated on 2 subtrees of the file system at the same time,
and required careful synchronization. The naive solution was to lock the root
of the file system. A better solution was to find the LCA of the two subtrees and lock it in `writer` mode (so that no other
thread can be reading anything in the whole directory). But to do it
without locking the whole subtree under the LCA, one had to very carefully DFS-traverse (not BFS! because it doesn't preserve the preorder) the tree,
locking the nodes in `reader` mode, then the target nodes in `writer` node. Doing it wrong resulted in a deadlock which happened *very* rarely, so every iteration of working on the synchronization algorithm required *extensive* testing :)

I have implemented my own version of `rwlock`, a lock which can be locked either in reader or writer mode.
Please take a look at the function which locks an `rwlock` in the `reader` mode:
Please also note that almost every information in the internet (including StackOverflow) regarding concurrent algorithms is wrong :) I relied on the notes from my university heavily while working on this project. This `rwlock` synchronization functions are not trivial, but thanks to that they are correct, i.e. there is no reader nor writer starvation. Doing this naively results in writers starvation! Here, the readers check if there is a writer waiting before they acquire the lock themselves :)
https://github.com/eerio/concurrent-file-system/blob/dc9a6e53d18537225a9ea7a7568ebadf8c3f6c71/rwlock.c#L38-L52

## Table of Contents

- [Introduction](#introduction)
- [Features](#features)
- [Contributing](#contributing)
- [License](#license)

## Introduction

The Tree-Based File System is a data structure that simulates a file system hierarchy. It allows the creation, deletion, and navigation of directories and files within a virtual file system. The implementation is designed to be thread-safe, allowing multiple threads to interact with the file system concurrently without data corruption or inconsistencies.

## Features

- Thread-safe operations: The file system is designed to be accessed and modified by multiple threads simultaneously while ensuring data integrity and consistency.
- Directory support: The file system supports the creation and management of directories, allowing for the organization of files in a hierarchical manner.
- File operations: No files! that's not a very usable file system :) it would be trivial to add them, but for educational purposes, everything is a directory
- Lightweight and efficient: The implementation is designed to be efficient, ensuring minimal overhead during operations.

## Contributing
Contributions to the Tree-Based File System project are welcome! If you find any bugs, have suggestions for improvements, or would like to add new features, please feel free to open an issue or submit a pull request.

When contributing to this repository, please ensure that your code follows the established coding conventions, and include appropriate tests to maintain the project's reliability.

## License
This project is licensed under the MIT License - see the LICENSE file for details.

We hope you find the Tree-Based File System useful for your needs. If you encounter any issues or have any questions, feel free to open an issue or contact the project maintainers.

Happy coding!
