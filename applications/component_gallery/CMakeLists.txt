add_executable(forma-components main.cpp)
target_link_libraries(forma-components PRIVATE mayonnaise::mayonnaise)
mayonnaise_set_warnings(forma-components)
mayonnaise_set_sanitizers(forma-components)
