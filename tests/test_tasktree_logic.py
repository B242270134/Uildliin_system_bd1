import unittest


def build_tree(parents):
    """PID 0-оос эхлэх мод байгуулна."""
    n = len(parents)
    parent_index = [-1] * n

    for i in range(1, n):
        p = parents[i]
        parent_index[i] = p if 0 <= p < n and p != i else 0

    for i in range(1, n):
        node = i
        steps = 0

        while node > 0 and steps < n:
            node = parent_index[node]
            steps += 1

        if node != 0:
            parent_index[i] = 0

    children = [[] for _ in range(n)]

    for i in range(1, n):
        children[parent_index[i]].append(i)

    return parent_index, children


def iterative_dfs(parents):
    parent_index, children = build_tree(parents)

    stack = [0]
    order = []
    depth = [-1] * len(parents)

    while stack:
        node = stack.pop()

        if depth[node] >= 0:
            continue

        parent = parent_index[node]

        if parent < 0:
            depth[node] = 0
        else:
            if depth[parent] < 0:
                continue
            depth[node] = depth[parent] + 1

        order.append(node)

        # LIFO стек ашиглаж байгаа тул урвуугаар нэмнэ.
        stack.extend(reversed(children[node]))

    return order, depth, parent_index


class TestTaskTree(unittest.TestCase):

    def test_simple_tree(self):
        parents = [-1, 0, 0, 1, 1, 2]
        order, depth, _ = iterative_dfs(parents)

        self.assertEqual(order, [0, 1, 3, 4, 2, 5])
        self.assertEqual(depth, [0, 1, 1, 2, 2, 2])

    def test_missing_parent(self):
        parents = [-1, 99, 1]
        order, depth, parent_index = iterative_dfs(parents)

        self.assertEqual(len(order), 3)
        self.assertEqual(parent_index[1], 0)
        self.assertEqual(depth[2], 2)

    def test_self_parent(self):
        parents = [-1, 1, 1]
        order, _, parent_index = iterative_dfs(parents)

        self.assertEqual(len(order), 3)
        self.assertEqual(parent_index[1], 0)

    def test_cycle(self):
        parents = [-1, 2, 1, 2]
        order, depth, _ = iterative_dfs(parents)

        self.assertEqual(len(order), 4)
        self.assertEqual(len(set(order)), 4)
        self.assertTrue(all(d >= 0 for d in depth))

    def test_deep_tree(self):
        n = 3000
        parents = [-1] + list(range(n - 1))

        order, depth, _ = iterative_dfs(parents)

        self.assertEqual(len(order), n)
        self.assertEqual(max(depth), n - 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
