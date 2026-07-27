if(NOT DEFINED STACK_ROOT OR NOT DEFINED MAX_SAVE_MESSAGE_STACK)
    message(FATAL_ERROR "STACK_ROOT and MAX_SAVE_MESSAGE_STACK are required")
endif()

file(GLOB_RECURSE STACK_USAGE_FILES "${STACK_ROOT}/*MessageStore.cpp.su")
if(NOT STACK_USAGE_FILES)
    message(FATAL_ERROR "MessageStore.cpp.su was not generated")
endif()

set(FOUND_SAVE_MESSAGE FALSE)
foreach(STACK_FILE IN LISTS STACK_USAGE_FILES)
    file(STRINGS "${STACK_FILE}" STACK_LINES)
    foreach(STACK_LINE IN LISTS STACK_LINES)
        string(REPLACE "\t" ";" STACK_FIELDS "${STACK_LINE}")
        list(LENGTH STACK_FIELDS STACK_FIELD_COUNT)
        if(STACK_FIELD_COUNT GREATER_EQUAL 3)
            list(GET STACK_FIELDS 0 STACK_SIGNATURE)
            list(GET STACK_FIELDS 1 STACK_BYTES)
            if(STACK_SIGNATURE MATCHES "bool LXMF::MessageStore::save_message")
                set(FOUND_SAVE_MESSAGE TRUE)
                if(STACK_BYTES GREATER MAX_SAVE_MESSAGE_STACK)
                    message(FATAL_ERROR
                        "MessageStore::save_message uses ${STACK_BYTES} bytes; "
                        "budget is ${MAX_SAVE_MESSAGE_STACK} bytes")
                endif()
                message(STATUS
                    "MessageStore::save_message stack: ${STACK_BYTES} bytes "
                    "(budget ${MAX_SAVE_MESSAGE_STACK})")
            endif()
        endif()
    endforeach()
endforeach()

if(NOT FOUND_SAVE_MESSAGE)
    message(FATAL_ERROR "save_message stack record was not found")
endif()
