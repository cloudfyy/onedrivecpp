import unittest
from pathlib import Path

from coverage_report import merge_lcov, summarize


class CoverageReportTests(unittest.TestCase):
    def test_combines_executable_lines_without_double_counting(self):
        files = {}
        root = Path("/project")
        merge_lcov(
            "SF:/project/src/core.cpp\nDA:10,0\nDA:11,1\n"
            "FNDA:0,function\nBRDA:10,0,0,-\nBRDA:10,0,1,1\nend_of_record\n",
            root, files,
        )
        merge_lcov(
            "SF:/project/src/core.cpp\nDA:10,3\nDA:11,0\n"
            "FNDA:3,function\nBRDA:10,0,0,3\nBRDA:10,0,1,-\nend_of_record\n",
            root, files,
        )
        entry = files["/project/src/core.cpp"]
        self.assertEqual(summarize(entry["lines"]), {
            "count": 2, "covered": 2, "percent": 100,
        })
        self.assertEqual(summarize(entry["branches"])["covered"], 2)
        self.assertEqual(summarize(entry["functions"])["count"], 1)

    def test_main_counters_are_separate_by_source_file(self):
        files = {}
        merge_lcov(
            "SF:/project/src/main.cpp\nDA:1,5\nFNDA:5,main\nend_of_record\n"
            "SF:/project/src/ui/gui/main.cpp\nDA:1,0\nFNDA:0,main\nend_of_record\n",
            Path("/project"), files,
        )
        self.assertEqual(summarize(files["/project/src/main.cpp"]["lines"])["covered"], 1)
        self.assertEqual(
            summarize(files["/project/src/ui/gui/main.cpp"]["lines"])["covered"], 0
        )

    def test_excludes_tests_dependencies_and_generated_sources(self):
        files = {}
        for path in (
            "/project/tests/core.cpp", "/usr/include/example.hpp",
            "/project/build/generated.cpp",
        ):
            merge_lcov(f"SF:{path}\nDA:1,1\n", Path("/project"), files)
        self.assertEqual(files, {})
        merge_lcov(
            "SF:/project/include/onedrive/core.hpp\nDA:1,0\n",
            Path("/project"), files,
        )
        self.assertEqual(len(files), 1)

    def test_handles_commas_in_function_names(self):
        files = {}
        merge_lcov(
            "SF:/project/src/core.cpp\nFNDA:1,function<int,float>\n",
            Path("/project"), files,
        )
        self.assertEqual(files["/project/src/core.cpp"]["functions"], {
            "function<int,float>": 1,
        })


if __name__ == "__main__":
    unittest.main()
