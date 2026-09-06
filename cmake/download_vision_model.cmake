if(NOT DEFINED MODEL_DIR)
    set(MODEL_DIR "${CMAKE_SOURCE_DIR}/assets/onnx/v1")
endif()

string(REPLACE "\"" "" MODEL_DIR "${MODEL_DIR}")

set(MODEL_URL
    "https://huggingface.co/onnx-community/siglip2-base-patch16-224-ONNX/resolve/main/onnx/vision_model_fp16.onnx?download=true"
)
set(MODEL_SHA256 "a1959f7bd3993a607e48839f6d01e25b876fe76afda301b028b78eef68aabd95")

set(MODEL_FILE "${MODEL_DIR}/vision_model_fp16.onnx")
file(MAKE_DIRECTORY "${MODEL_DIR}")

if(EXISTS "${MODEL_FILE}")
    file(SHA256 "${MODEL_FILE}" EXISTING_HASH)
    if(EXISTING_HASH STREQUAL MODEL_SHA256)
        message(STATUS "Vision model already present: ${MODEL_FILE}")
        return()
    endif()
    message(STATUS "Existing vision model hash mismatch, re-downloading...")
endif()

message(STATUS "Downloading SigLIP2 vision model (~186 MB)...")
file(DOWNLOAD
    "${MODEL_URL}"
    "${MODEL_FILE}"
    SHOW_PROGRESS
    TLS_VERIFY ON
    EXPECTED_HASH "SHA256=${MODEL_SHA256}"
)
message(STATUS "Vision model downloaded: ${MODEL_FILE}")
