load("//tensorflow:tensorflow.default.bzl", "pybind_extension")

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    default_visibility = ["//tensorflow:__subpackages__"],
    licenses = ["notice"],
)

pybind_extension(
    name = "pybind_for_testing",
    srcs = ["pybind_for_testing.cc"],
    enable_stub_generation = True,
    pytype_srcs = [
        "pybind_for_testing.pyi",
    ],
    deps = [
        "@pybind11",
    ],
)
