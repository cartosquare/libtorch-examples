#include <c10d/ProcessGroupGloo.hpp>
#include <c10d/ProcessGroupNCCL.hpp>
#include <c10d/TCPStore.hpp>
#include <torch/torch.h>

#include <memory>
#include <iostream>

// Define a Convolutional Module
struct Model : torch::nn::Module {
  Model()
      : conv1(torch::nn::Conv2dOptions(1, 10, 5)),
        conv2(torch::nn::Conv2dOptions(10, 20, 5)),
        fc1(320, 50),
        fc2(50, 10) {
    register_module("conv1", conv1);
    register_module("conv2", conv2);
    register_module("conv2_drop", conv2_drop);
    register_module("fc1", fc1);
    register_module("fc2", fc2);
  }

  torch::Tensor forward(torch::Tensor x) {
    x = torch::relu(torch::max_pool2d(conv1->forward(x), 2));
    x = torch::relu(
        torch::max_pool2d(conv2_drop->forward(conv2->forward(x)), 2));
    x = x.view({-1, 320});
    x = torch::relu(fc1->forward(x));
    x = torch::dropout(x, 0.5, is_training());
    x = fc2->forward(x);
    return torch::log_softmax(x, 1);
  }

  torch::nn::Conv2d conv1;
  torch::nn::Conv2d conv2;
  torch::nn::Dropout2d conv2_drop;
  torch::nn::Linear fc1;
  torch::nn::Linear fc2;
};

void waitWork(
    std::shared_ptr<c10d::ProcessGroup> pg,
    std::vector<c10::intrusive_ptr<c10d::ProcessGroup::Work>> works) {
  for (auto& work : works) {
    try {
      work->wait();
    } catch (const std::exception& ex) {
      std::cerr << "Exception received: " << ex.what() << std::endl;
      //pg->abort();
    }
  }
}

std::vector<std::string> split(char separator, const std::string& string) {
  std::vector<std::string> pieces;
  std::stringstream ss(string);
  std::string item;
  while (std::getline(ss, item, separator)) {
    pieces.push_back(std::move(item));
  }
  return pieces;
}

int main(int argc, char* argv[]) {
  auto master_addr = getenv("MASTER_ADDR");
  auto master_port = atoi(getenv("MASTER_PORT"));
  auto size = atoi(getenv("SIZE"));
  auto rank = atoi(getenv("RANK"));
  std::string backend = getenv("BACKEND");
  std::string device = getenv("DEVICE");
  
  torch::DeviceType device_type = torch::kCPU;
  if (backend == "nccl") {
    device = "cuda";
  }
  if (device == "cuda") {
    device_type = torch::kCUDA;
  }

  std::cout << "master: " << master_addr << std::endl;
  std::cout << "port: " << master_port << std::endl;
  std::cout << "world size: " << size << std::endl;
  std::cout << "rank: " << rank << std::endl;
  std::cout << "backend: " << backend << std::endl;
  std::cout << "device: " << device_type << std::endl;

  std::shared_ptr<c10d::ProcessGroup> pg;
  auto store = c10::make_intrusive<c10d::TCPStore>(master_addr, master_port, size, rank == 0);
  if (backend == "gloo") {
    c10d::ProcessGroupGloo::Options options;
    options.timeout = std::chrono::milliseconds(100000);

    char* ifnameEnv = getenv("GLOO_SOCKET_IFNAME");
    if (ifnameEnv) {
        for (const auto& iface : split(',', ifnameEnv)) {
            options.devices.push_back(c10d::ProcessGroupGloo::createDeviceForInterface(iface));
        }
    } else {
        // If no hostname is specified, this function looks up
        // the machine's hostname and returns a device instance
        // associated with the address that the hostname resolves to.
        options.devices.push_back(c10d::ProcessGroupGloo::createDefaultDevice());
    }

    std::cout << "#devices: " << options.devices.size() << std::endl;
    pg = std::shared_ptr<c10d::ProcessGroup>(new c10d::ProcessGroupGloo(store, rank, size, options));
  } else {
    std::cout << "nccl progress group\n";
    pg = std::shared_ptr<c10d::ProcessGroup>(new c10d::ProcessGroupNCCL(store, rank, size));
  }

  // TRAINING
  // Read train dataset
  const char* kDataRoot = "../data/mnist";
  auto train_dataset =
      torch::data::datasets::MNIST(kDataRoot)
          .map(torch::data::transforms::Normalize<>(0.1307, 0.3081))
          .map(torch::data::transforms::Stack<>());

  // Distributed Random Sampler
  auto data_sampler = torch::data::samplers::DistributedRandomSampler(
      train_dataset.size().value(), size, rank, false);

  auto num_train_samples_per_proc = train_dataset.size().value() / size;

  // Generate dataloader
  auto total_batch_size = 64;
  auto batch_size_per_proc =
      total_batch_size / size; // effective batch size in each processor
  auto data_loader = torch::data::make_data_loader(
      std::move(train_dataset), data_sampler, batch_size_per_proc);

  // setting manual seed
  torch::manual_seed(0);

  auto model = std::make_shared<Model>();
  model->to(device_type);

  auto learning_rate = 1e-2;

  torch::optim::SGD optimizer(model->parameters(), learning_rate);

  // Number of epochs
  size_t num_epochs = 10;

  std::cout << "begin epoch ...\n";
  for (size_t epoch = 1; epoch <= num_epochs; ++epoch) {
    size_t num_correct = 0;

    for (auto& batch : *data_loader) {
      auto ip = batch.data;
      auto op = batch.target.squeeze();

      // convert to required formats
      ip = ip.to(torch::kF32).to(device_type);
      op = op.to(torch::kLong).to(device_type);

      // Reset gradients
      model->zero_grad();

      // Execute forward pass
      auto prediction = model->forward(ip);

      auto loss = torch::nll_loss(torch::log_softmax(prediction, 1), op);

      // Backpropagation
      loss.backward();

      // Averaging the gradients of the parameters in all the processors
      // Note: This may lag behind DistributedDataParallel (DDP) in performance
      // since this synchronizes parameters after backward pass while DDP
      // overlaps synchronizing parameters and computing gradients in backward
      // pass
      std::vector<c10::intrusive_ptr<c10d::ProcessGroup::Work>> works;
      for (auto& param : model->named_parameters()) {
        std::vector<torch::Tensor> tmp = {param.value().grad()};
        auto work = pg->allreduce(tmp);
        works.push_back(std::move(work));
      }

      waitWork(pg, works);

      for (auto& param : model->named_parameters()) {
        param.value().grad().data() = param.value().grad().data() / size;
      }

      // Update parameters
      optimizer.step();

      auto guess = prediction.argmax(1);
      num_correct += torch::sum(guess.eq_(op)).item<int64_t>();
    } // end batch loader

    auto accuracy = 100.0 * num_correct / num_train_samples_per_proc;

    std::cout << "Accuracy in rank " << rank << " in epoch " << epoch << " - "
              << accuracy << std::endl;

  } // end epoch

  // TESTING ONLY IN RANK 0
  if (rank == 0) {
    auto test_dataset =
        torch::data::datasets::MNIST(
            kDataRoot, torch::data::datasets::MNIST::Mode::kTest)
            .map(torch::data::transforms::Normalize<>(0.1307, 0.3081))
            .map(torch::data::transforms::Stack<>());

    auto num_test_samples = test_dataset.size().value();
    auto test_loader = torch::data::make_data_loader(
        std::move(test_dataset), num_test_samples);

    model->eval(); // enable eval mode to prevent backprop

    size_t num_correct = 0;

    for (auto& batch : *test_loader) {
      auto ip = batch.data;
      auto op = batch.target.squeeze();

      // convert to required format
      ip = ip.to(torch::kF32).to(device_type);
      op = op.to(torch::kLong).to(device_type);

      auto prediction = model->forward(ip);

      auto loss = torch::nll_loss(torch::log_softmax(prediction, 1), op);

      std::cout << "Test loss - " << loss.item<float>() << std::endl;

      auto guess = prediction.argmax(1);

      num_correct += torch::sum(guess.eq_(op)).item<int64_t>();

    } // end test loader

    std::cout << "Num correct - " << num_correct << std::endl;
    std::cout << "Test Accuracy - " << 100.0 * num_correct / num_test_samples
              << std::endl;
  } // end rank 0
}
