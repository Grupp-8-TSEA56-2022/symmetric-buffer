#include <string>
#include <iostream>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdarg>

#include "pipeline.h"
#include "logging.h"


using namespace std;

GenericStage::GenericStage(pipeline_link &link_out)
: link_out{link_out}, stage_index{}, th{}, running{true} {};

GenericStage::~GenericStage() {
    running.store(false);  // thread will stop when current cycle complete
    if (link_out.mtx.try_lock()) {
        // Pretend to clear buffer so that worker wont lock at it next time
        // (this introduces a memory leak, in the worst case it leaks
        // number_of_workers*2*data_size) I don't know what to do about it.
        link_out.has_data = false;

        link_out.mtx.unlock();
        link_out.cv.notify_all();  // in case worker was waiting
    } else {
        // Worker will complete this cycle without our intervention
    }
    th->join();  // this could have deadlocked if we didn't clear the buffer
    delete th;
}


FirstStage::FirstStage(function<void*()> func, pipeline_link &link_out)
: GenericStage(link_out), func{func} {
    stage_index = 0;
    th = new thread(&FirstStage::run, this);
}

Stage::Stage(
    function<void*(void*)> func,
    pipeline_link &link_in,
    pipeline_link &link_out,
    unsigned int stage_idx
): GenericStage(link_out), link_in{link_in}, func{func} {
    stage_index = stage_idx;
    th = new thread(&Stage::run, this);
}

void FirstStage::run() {
    while (running.load()) {
        log_worker("Start worker %d\n", stage_index);
        const auto start_time = chrono::high_resolution_clock::now();
        void* data = func();
        const auto stop_time = chrono::high_resolution_clock::now();
        double duration = chrono::duration<double, std::milli>(stop_time-start_time).count();
        log_worker("\tWorker %d done (ran for %.2f ms)\n", stage_index, duration);
        {
            unique_lock<mutex> lk(link_out.mtx);
            link_out.cv.wait(lk, [this]{return !link_out.has_data;});
            link_out.buffer = std::move(data);
            link_out.has_data = true;
        }
        link_out.cv.notify_one();
    }
}

void Stage::run() {
    while (running.load()) {
        void* data;
        const auto start_wait = chrono::high_resolution_clock::now();
        {
            unique_lock<mutex> lk(link_in.mtx);
            link_in.cv.wait(lk, [this]{return link_in.has_data;});
            data = std::move(link_in.buffer);
            link_in.has_data = false;
        }
        link_in.cv.notify_one();
        const auto stop_wait = chrono::high_resolution_clock::now();
        double wait_dur = chrono::duration<double, std::milli>(stop_wait-start_wait).count();

        log_worker("Start worker %d (waited %.2f ms)\n", stage_index, wait_dur);
        const auto start_work = chrono::high_resolution_clock::now();
        data = func(data);
        const auto stop_work = chrono::high_resolution_clock::now();
        double work_dur = chrono::duration<double, std::milli>(stop_work-start_work).count();
        log_worker("\tWorker %d done (ran for %.2f ms)\n", stage_index, work_dur);

        {
            unique_lock<mutex> lk(link_out.mtx);
            link_out.cv.wait(lk, [this]{return !link_out.has_data;});
            link_out.buffer = std::move(data);
            link_out.has_data = true;
        }
        link_out.cv.notify_one();
    }
}

