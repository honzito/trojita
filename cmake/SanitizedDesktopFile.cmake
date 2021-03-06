macro(copy_desktop_file_without_cruft SOURCE TARGET)
    file(READ "${SOURCE}" orig_content)
    string(REGEX REPLACE "([a-zA-Z]+\\[x-test\\]=[^\n]+\n)" "" sanitized_content "${orig_content}")
    file(WRITE "${TARGET}" "${sanitized_content}")
endmacro()
