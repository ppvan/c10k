
# Heap

Min-Heap is a tree data structure that maintain following property:
- Is a complete binary tree [def](https://en.wikipedia.org/wiki/Binary_tree#Types_of_binary_trees)
- The parent always <= any of its children

Heap operations:
1. peek() -> see the root
2. insert() -> insert new values to the tree
3. pop() -> get the root values and remove it from the tree.

4. _heapify_down()
5. _heapify_up()

Remember that the heap must always maintain its property, hence each op has to make sure the property is kept. The functions `heapify_*` is doing that work, but often perform automtically after insert or pop


