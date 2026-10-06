# obe_add_app(<name> <sources...>)
function(obe_add_app name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE obe::obe)
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps")
endfunction()

obe_add_app(itch_stats itch_stats.cpp)
obe_add_app(itch_synth itch_synth.cpp)
obe_add_app(book_replay book_replay.cpp)
obe_add_app(book_view book_view.cpp)
obe_add_app(feed_profile feed_profile.cpp)
obe_add_app(flow_gen flow_gen.cpp)

# The exchange (phase 7): order entry over TCP, market data over UDP. Linux only.
obe_add_app(exchange_server exchange_server.cpp)
obe_add_app(load_gen load_gen.cpp)
obe_add_app(md_listen md_listen.cpp)

# The market-making simulator (phase 8): a replay with simulated quotes.
obe_add_app(mm_sim mm_sim.cpp)
