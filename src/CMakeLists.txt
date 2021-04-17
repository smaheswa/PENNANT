set(PENNANT_LIB_SRC
    Driver.cc 
    ExportGold.cc 
    GenMesh.cc 
    Hydro.cc 
    HydroBC.cc  
    InputFile.cc 
    Mesh.cc 
    Parallel.cc 
    PolyGas.cc 
    QCS.cc 
    TTS.cc 
    WriteXY.cc 
)
add_library(libpennant STATIC ${PENNANT_LIB_SRC})
set_target_properties(libpennant PROPERTIES OUTPUT_NAME pennant)
target_include_directories(libpennant PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_include_directories(libpennant PUBLIC ${MPI_CXX_INCLUDE_DIRS})
target_link_libraries(libpennant PUBLIC ${MPI_CXX_LIBRARIES})