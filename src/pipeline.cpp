#include <mutex>
#include <future>
#include <thread>
#include <iostream>

#include "pipeline.h"

using namespace std;

Pipeline::Pipeline(function<void*()> f, initializer_list<function<void*(void*)>> f_list): pipeline_links{}, stages{} {
    for (pipeline_link &pl : pipeline_links )
        pl.has_data = false;
    stages.push_back(new FirstStage(f, pipeline_links[0]));
    int i{0};
    for (auto fn : f_list) {
        stages.push_back(new Stage(fn, pipeline_links[i], pipeline_links[i+1], i+1));
        ++i;
    }
}

Pipeline::~Pipeline() {
    for (auto stage : stages)
        delete stage;
}

future<void*> Pipeline::get_future() {
    pipeline_link &link = pipeline_links[stages.size()-1];
    void *data;
    {
        unique_lock<mutex> lk(link.mtx);
        link.cv.wait(lk, [this]{return pipeline_links[stages.size()-1].has_data;});
        data = std::move(link.buffer);
        link.has_data = false;
    }
    link.cv.notify_one();

    promise<void*> p;
    p.set_value(data);
    return p.get_future();
}
