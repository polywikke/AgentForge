// examples/quick_sort.cpp
// 快速排序的几种实现与对比，C++20 单文件，可直接编译运行：
//   g++ -std=c++20 -O2 -Wall -Wextra examples/quick_sort.cpp -o bin/quick_sort.exe
//
// 包含：
//   1. quick_sort        —— 主实现：三数取中 + 小数组插入排序 + 尾递归消除
//   2. quick_sort_three_way —— 三路分区（荷兰国旗），适合大量重复元素
//   3. quick_sort_iterative —— 显式栈版本，彻底避免深递归爆栈
//   4. 自测：与 std::sort 对拍 + 边界/重复/有序等特例

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <stack>
#include <string>
#include <utility>
#include <vector>

namespace qs {

// 小于该长度的子区间改用插入排序：小数组上插入排序常数更小，比继续递归更快
constexpr int kInsertionThreshold = 16;

template <typename T>
void insertionSort(std::vector<T>& a, int lo, int hi) {
    for (int i = lo + 1; i <= hi; ++i) {
        T key = std::move(a[i]);
        int j = i - 1;
        while (j >= lo && a[j] > key) {
            a[j + 1] = std::move(a[j]);
            --j;
        }
        a[j + 1] = std::move(key);
    }
}

// 三数取中：在 lo / mid / hi 三个位置里选中间值作为枢轴，
// 避免对已排序或逆序数组退化为 O(n^2)。
inline int medianOfThree(std::vector<int>& a, int lo, int hi) {
    int mid = lo + (hi - lo) / 2;
    if (a[mid] < a[lo]) std::swap(a[mid], a[lo]);
    if (a[hi] < a[lo])  std::swap(a[hi], a[lo]);
    if (a[hi] < a[mid]) std::swap(a[hi], a[mid]);
    // 现在 a[lo] <= a[mid] <= a[hi]
    std::swap(a[mid], a[hi - 1]);   // 把中位数藏到 hi-1 位置当哨兵
    return a[hi - 1];
}

// Hoare 风格分区（双指针向中间靠拢），返回枢轴最终落点。
// 相比 Lomuto 分区，平均交换次数约为其 1/3。
// 前提：区间长度 >= 3，且 a[hi-1] 已是由 medianOfThree 放好的枢轴。
inline int partition(std::vector<int>& a, int lo, int hi) {
    int pivot = medianOfThree(a, lo, hi);
    int i = lo;
    int j = hi - 1;
    while (true) {
        while (a[++i] < pivot) {}       // 左指针找 >= pivot 的元素
        while (pivot < a[--j]) {}       // 右指针找 <= pivot 的元素
        if (i >= j) break;
        std::swap(a[i], a[j]);
    }
    std::swap(a[i], a[hi - 1]);         // 枢轴归位到 i
    return i;
}

// 主实现：递归 + 尾递归消除 + 小数组插入排序
template <typename T>
void quickSortImpl(std::vector<T>& a, int lo, int hi) {
    while (lo < hi) {
        if (hi - lo + 1 <= kInsertionThreshold) {
            insertionSort(a, lo, hi);
            return;
        }
        int p = partition(a, lo, hi);
        // 关键：只对较短的一侧递归，较长的一侧用循环处理。
        // 这样递归深度上界是 O(log n)，最坏输入也不会栈溢出。
        if (p - lo < hi - p) {
            quickSortImpl(a, lo, p - 1);
            lo = p + 1;
        } else {
            quickSortImpl(a, p + 1, hi);
            hi = p - 1;
        }
    }
}

// 对外接口：对 [0, n-1] 升序排序
template <typename T>
void quickSort(std::vector<T>& a) {
    if (a.size() < 2) return;
    quickSortImpl(a, 0, static_cast<int>(a.size()) - 1);
}

// ---------------------------------------------------------------------------
// 三路分区（荷兰国旗）：把区间切成 < pivot | == pivot | > pivot 三段。
// 当数组里有大量重复键时，这是唯一能保持 O(n log n) 的形态；
// 两路分区在"全部元素相同"的输入上会退化。
// ---------------------------------------------------------------------------
template <typename T>
void quickSortThreeWayImpl(std::vector<T>& a, int lo, int hi) {
    if (lo >= hi) return;
    if (hi - lo + 1 <= kInsertionThreshold) {
        insertionSort(a, lo, hi);
        return;
    }

    T pivot = a[lo + (hi - lo) / 2];
    int lt = lo;        // a[lo..lt-1] < pivot
    int i = lo;         // a[lt..i-1] == pivot
    int gt = hi;        // a[gt+1..hi] > pivot
    while (i <= gt) {
        if (a[i] < pivot) {
            std::swap(a[lt++], a[i++]);
        } else if (pivot < a[i]) {
            std::swap(a[i], a[gt--]);
        } else {
            ++i;
        }
    }
    // 中间段 [lt..gt] 已全部等于 pivot，无需再排
    quickSortThreeWayImpl(a, lo, lt - 1);
    quickSortThreeWayImpl(a, gt + 1, hi);
}

template <typename T>
void quickSortThreeWay(std::vector<T>& a) {
    if (a.size() < 2) return;
    quickSortThreeWayImpl(a, 0, static_cast<int>(a.size()) - 1);
}

// ---------------------------------------------------------------------------
// 迭代版：用显式栈保存待处理区间，函数栈深度恒定为 O(1)。
// 适合不能接受任何递归风险的环境（如嵌入式、深递归受限的线程）。
// ---------------------------------------------------------------------------
template <typename T>
void quickSortIterative(std::vector<T>& a) {
    if (a.size() < 2) return;
    std::vector<std::pair<int, int>> stk;
    stk.emplace_back(0, static_cast<int>(a.size()) - 1);

    while (!stk.empty()) {
        auto [lo, hi] = stk.back();
        stk.pop_back();
        if (hi - lo + 1 <= kInsertionThreshold) {
            insertionSort(a, lo, hi);
            continue;
        }
        int p = partition(a, lo, hi);
        // 先压长的、后压短的，让短的先出栈 → 栈内元素数保持 O(log n)
        int left_lo = lo, left_hi = p - 1;
        int right_lo = p + 1, right_hi = hi;
        if (left_hi - left_lo > right_hi - right_lo) {
            std::swap(left_lo, right_lo);
            std::swap(left_hi, right_hi);
        }
        if (left_lo < left_hi)   stk.emplace_back(left_lo, left_hi);
        if (right_lo < right_hi) stk.emplace_back(right_lo, right_hi);
    }
}

}  // namespace qs

// ---------------------------------------------------------------------------
// 自测
// ---------------------------------------------------------------------------
namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& name) {
    if (ok) {
        ++g_pass;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        ++g_fail;
        std::cout << "  [FAIL] " << name << "\n";
    }
}

// 用三种实现分别排序，再与 std::sort 的结果对拍
bool verifyAll(const std::vector<int>& input, const std::string& name) {
    std::vector<int> expected = input;
    std::sort(expected.begin(), expected.end());

    auto a1 = input; qs::quickSort(a1);
    auto a2 = input; qs::quickSortThreeWay(a2);
    auto a3 = input; qs::quickSortIterative(a3);

    bool ok = (a1 == expected) && (a2 == expected) && (a3 == expected);
    check(ok, name + " (n=" + std::to_string(input.size()) + ")");
    return ok;
}

void printArray(const std::vector<int>& a, const std::string& label) {
    std::cout << label << " [";
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::cout << a[i] << (i + 1 < a.size() ? ", " : "");
    }
    std::cout << "]\n";
}

}  // namespace

int main() {
    std::cout << "================ 快速排序演示 ================\n\n";

    // ---- 1. 直观演示 ----
    std::vector<int> demo = {38, 27, 43, 3, 9, 82, 10, 27, 1, 56};
    printArray(demo, "排序前:");
    qs::quickSort(demo);
    printArray(demo, "排序后:");

    std::cout << "\n---- 自测 ----\n";

    // ---- 2. 边界情况 ----
    verifyAll({}, "空数组");
    verifyAll({7}, "单元素");
    verifyAll({2, 1}, "两元素逆序");
    verifyAll({1, 2}, "两元素已序");

    // ---- 3. 典型形态 ----
    verifyAll({5, 5, 5, 5, 5, 5, 5, 5}, "全部相同");
    verifyAll({9, 8, 7, 6, 5, 4, 3, 2, 1}, "完全逆序");
    verifyAll({1, 2, 3, 4, 5, 6, 7, 8, 9}, "完全有序");
    verifyAll({-5, 100, 0, -100, 42, -42}, "含负数");

    // ---- 4. 随机对拍：多种规模 ----
    std::mt19937 rng(20240607);
    for (int n : {17, 100, 1000, 20000}) {
        std::uniform_int_distribution<int> dist(-10000, 10000);
        std::vector<int> v(n);
        for (auto& x : v) x = dist(rng);
        verifyAll(v, "随机数组");
    }

    // ---- 5. 大量重复键：验证三路分区的价值 ----
    {
        std::uniform_int_distribution<int> dist(0, 3);   // 只有 4 种取值
        std::vector<int> v(50000);
        for (auto& x : v) x = dist(rng);
        verifyAll(v, "大量重复键");
    }

    // ---- 6. 极端：已经有序的 20 万元素，验证三数取中不会退化 ----
    {
        std::vector<int> v(200000);
        for (int i = 0; i < static_cast<int>(v.size()); ++i) v[i] = i;
        verifyAll(v, "有序大数组（防退化）");
    }

    // ---- 7. 迭代版能处理递归版会爆栈的深度 ----
    {
        std::vector<int> v(300000);
        for (int i = 0; i < static_cast<int>(v.size()); ++i) v[i] = -i;
        qs::quickSortIterative(v);
        check(std::is_sorted(v.begin(), v.end()), "迭代版排序 30 万逆序元素");
    }

    std::cout << "\n================ 结果 ================\n";
    std::cout << "通过 " << g_pass << " 项，失败 " << g_fail << " 项\n";
    return g_fail == 0 ? 0 : 1;
}
