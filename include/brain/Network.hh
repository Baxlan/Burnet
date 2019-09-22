#ifndef BRAIN_NETWORK_HH_
#define BRAIN_NETWORK_HH_

#include "Layer.hh"
#include "preprocess.hh"
#include "cost.hh"
#include "annealing.hh"
#include "test.hh"

#include <iostream>

namespace brain
{

enum class Loss {L1, L2, CrossEntropy, BinaryCrossEntropy};
enum class Cost {L1, L2, Accuracy};
typedef std::vector<std::pair<std::vector<double>, std::vector<double>>> Dataset;

//=============================================================================
//=============================================================================
//=============================================================================
//=== NETWORK PARAMETERS ======================================================
//=============================================================================
//=============================================================================
//=============================================================================


struct NetworkParam
{
    NetworkParam():
    seed(0),
    batchSize(1),
    learningRate(0.001),
    L1(0),
    L2(0),
    epoch(50),
    patience(5),
    dropout(0),
    dropconnect(0),
    validationRatio(0.2),
    testRatio(0.2),
    loss(Loss::L2),
    LRDecayConstant(0.01),
    LRStepDecay(10),
    decay(LRDecay::none),
    classValidity(0.9),
    threads(1),
    optimizer(Optimizer::None),
    momentum(0.9),
    window(0.9),
    metric(Cost::L1),
    plateau(0.999),
    normalizeOutputs(false)
    {
    }

    unsigned seed;
    unsigned batchSize;
    double learningRate;
    double L1;
    double L2;
    unsigned epoch;
    unsigned patience;
    double dropout;
    double dropconnect;
    double validationRatio;
    double testRatio;
    Loss loss;
    double LRDecayConstant;
    unsigned LRStepDecay;
    double (* decay)(double, unsigned, double, unsigned);
    double classValidity; // %
    unsigned threads;
    Optimizer optimizer;
    double momentum; //momentum
    double window; //window effect on grads
    Cost metric;
    double plateau;
    bool normalizeOutputs;
};



//=============================================================================
//=============================================================================
//=============================================================================
//=== NETWORK =================================================================
//=============================================================================
//=============================================================================
//=============================================================================


class Network
{
public:
  Network(std::vector<std::string> const& labels, NetworkParam const& param = NetworkParam()):
  _seed(param.seed == 0 ? static_cast<unsigned>(std::chrono::steady_clock().now().time_since_epoch().count()) : param.seed),
  _generator(std::mt19937(_seed)),
  _dropoutDist(std::bernoulli_distribution(param.dropout)),
  _dropconnectDist(std::bernoulli_distribution(param.dropconnect)),
  _layers(),
  _pool(param.threads),
  _LRDecayConstant(param.LRDecayConstant),
  _LRStepDecay(param.LRStepDecay),
  _decay(param.decay),
  _batchSize(param.batchSize),
  _learningRate(param.learningRate),
  _L1(param.L1),
  _L2(param.L2),
  _dropout(param.dropout),
  _dropconnect(param.dropconnect),
  _maxEpoch(param.epoch),
  _patience(param.patience),
  _loss(param.loss),
  _validationRatio(param.validationRatio),
  _testRatio(param.testRatio),
  _rawData(),
  _trainData(),
  _validationData(),
  _validationRealResults(),
  _testData(),
  _testRealResults(),
  _nbBatch(0),
  _epoch(0),
  _optimalEpoch(0),
  _trainLosses(),
  _validLosses(),
  _testMetric(),
  _testSecondMetric(),
  _classValidity(param.classValidity),
  _optimizer(param.optimizer),
  _momentum(param.momentum),
  _window(param.window),
  _metric(param.metric),
  _labels(labels),
  _plateau(param.plateau), //if loss * plateau >= optimalLoss after "patience" epochs, end learning
  _normalizeOutputs(param.normalizeOutputs),
  _outputMeans()
  {
  }


  template <typename Aggr_t = Dot, typename Act_t = Relu>
  void addLayer(LayerParam const& param = LayerParam())
  {
    _layers.push_back(std::make_shared<Layer<Aggr_t, Act_t>>(_pool, param));
  }


  void setData(Dataset const& data)
  {
    _rawData = data;
  }


  //should take Dataset
  void setValidData(Matrix const& inputs, Matrix const& outputs)
  {
    _validationData = inputs;
    _validationRealResults = outputs;
  }


  //should take Dataset
  void setTestData(Matrix const& inputs, Matrix const& outputs)
  {
    _testData = inputs;
    _testRealResults = outputs;
  }


  bool learn()
  {
    initLayers();
    shuffleData();
    check();

    auto a = standardize(_trainData);
    standardize(_validationData, a);
    standardize(_testData, a);

    if(_normalizeOutputs)
    {
      auto b = normalize(_trainRealResults);
      normalize(_validationRealResults, b);
      normalize(_testRealResults, b);
    }

    if(_layers[_layers.size()-1]->size() != _trainRealResults[0].size())
    {
      throw Exception("The last layer must have as much neurons as outputs.");
    }

    double lowestLoss = computeLoss();
    std::cout << "\n";
    for(_epoch = 1; _epoch < _maxEpoch; _epoch++)
    {
      performeOneEpoch();

      std::cout << "Epoch: " << _epoch;
      double validLoss = computeLoss();
      std::cout << "   LR: " << _decay(_learningRate, _epoch, _LRDecayConstant, _LRStepDecay) << "\n";
      if(std::isnan(_trainLosses[_epoch]) || std::isnan(validLoss))
      {
        return false;
      }
      //EARLY STOPPING
      if(validLoss < lowestLoss * _plateau) //if loss increases, or doesn't decrease more than 0.5% in _patience epochs, stop learning
      {
        save();
        lowestLoss = validLoss;
        _optimalEpoch = _epoch;
      }
      if(_epoch - _optimalEpoch > _patience)
        break;
    }
    loadSaved();
    std::cout << "\nOptimal epoch: " << _optimalEpoch << "   First metric: " << _testMetric[_optimalEpoch] << "   Second metric: " << _testSecondMetric[_optimalEpoch] << "\n";
    return true;
  }


  Matrix process(Matrix inputs) const
  {
    for(unsigned i = 0; i < _layers.size(); i++)
    {
      inputs = _layers[i]->process(inputs, _pool);
    }
    // if cross-entropy loss is used, then score must be softmax
    if(_loss == Loss::CrossEntropy)
    {
      inputs = softmax(inputs);
    }
    return inputs;
  }


  void writeInfo(std::string const& path) const
  {
    std::pair<std::vector<double>, std::vector<double>> acc;
    std::string metric;
    if(_metric == Cost::Accuracy)
    {
      acc = accuracyPerOutput(_testRealResults, process(_testData), _classValidity);
      metric = "accuracy";
    }
    else if(_metric == Cost::L1)
    {
      acc = L1CostPerOutput(_testRealResults, process(_testData));
      metric = "mae";
    }
    if(_metric == Cost::L2)
    {
      acc = L2CostPerOutput(_testRealResults, process(_testData));
      metric = "mse";
    }
    std::ofstream output(path);
    for(unsigned i=0; i<_labels.size(); i++)
    {
        output << _labels[i] << ",";
    }
    output << "\n";
    for(unsigned i=0; i<_trainLosses.size(); i++)
    {
        output << _trainLosses[i] << ",";
    }
    output << "\n";
    for(unsigned i=0; i<_validLosses.size(); i++)
    {
        output << _validLosses[i] << ",";
    }
    output << "\n" << metric << "\n";
    for(unsigned i=0; i<_testMetric.size(); i++)
    {
        output << _testMetric[i] << ",";
    }
    output << "\n";
    for(unsigned i=0; i<_testSecondMetric.size(); i++)
    {
        output << _testSecondMetric[i] << ",";
    }
    output << "\n";
    for(unsigned i=0; i<acc.first.size(); i++)
    {
        output << acc.first[i] << ",";
    }
        output << "\n";
    for(unsigned i=0; i<acc.second.size(); i++)
    {
        output << acc.second[i] << ",";
    }
    output << "\n";
    output << _optimalEpoch;
  }


protected:
  void initLayers()
  {
    for(unsigned i = 0; i < _layers.size(); i++)
    {
        _layers[i]->init((i == 0 ? _rawData[0].first.size() : _layers[i-1]->size()),
                        (i == _layers.size()-1 ? _rawData[0].second.size() : _layers[i+1]->size()),
                        _batchSize);
    }
  }


  void shuffleData()
  {
    std::shuffle(_rawData.begin(), _rawData.end(), _generator);

    double validation = _validationRatio * _rawData.size();
    double test = _testRatio * _rawData.size();
    double nbBatch = std::trunc(_rawData.size() - validation - test) / _batchSize;

    //add a batch if an incomplete batch has more than 0.5*batchsize data
    if(nbBatch - static_cast<unsigned>(nbBatch) >= 0.5)
      nbBatch = std::trunc(nbBatch) + 1;

    unsigned nbTrain = static_cast<unsigned>(nbBatch)*_batchSize;
    unsigned noTrain = _rawData.size() - nbTrain;
    validation = std::round(noTrain*_validationRatio/(_validationRatio + _testRatio));
    test = std::round(noTrain*_testRatio/(_validationRatio + _testRatio));

    for(unsigned i = 0; i < static_cast<unsigned>(validation); i++)
    {
      _validationData.push_back(_rawData[_rawData.size()-1].first);
      _validationRealResults.push_back(_rawData[_rawData.size()-1].second);
      _rawData.pop_back();
    }
    for(unsigned i = 0; i < static_cast<unsigned>(test); i++)
    {
      _testData.push_back(_rawData[_rawData.size()-1].first);
      _testRealResults.push_back(_rawData[_rawData.size()-1].second);
      _rawData.pop_back();
    }
    unsigned size = _rawData.size();
    for(unsigned i = 0; i < size; i++)
    {
      _trainData.push_back(_rawData[_rawData.size()-1].first);
      _trainRealResults.push_back(_rawData[_rawData.size()-1].second);
      _rawData.pop_back();
    }
    _nbBatch = static_cast<unsigned>(nbBatch);
  }


  void check() const
  {

  }


  void performeOneEpoch()
  {
    for(unsigned batch = 0; batch < _nbBatch; batch++)
    {
      Matrix input(_batchSize);
      Matrix output(_batchSize);
      for(unsigned i = 0; i < _batchSize; i++)
      {
        input[i] = _trainData[batch*_batchSize+i];
        output[i] = _trainRealResults[batch*_batchSize+i];
      }

      for(unsigned i = 0; i < _layers.size(); i++)
      {
        input = _layers[i]->processToLearn(input, _dropout, _dropconnect, _dropoutDist, _dropconnectDist, _generator, _pool);
      }

      Matrix gradients(transpose(computeLossMatrix(output, input).second));
      for(unsigned i = 0; i < _layers.size(); i++)
      {
        _layers[_layers.size() - i - 1]->computeGradients(gradients, _pool);
        gradients = _layers[_layers.size() - i - 1]->getGradients();
      }
      for(unsigned i = 0; i < _layers.size(); i++)
      {
        _layers[i]->updateWeights(_decay(_learningRate, _epoch, _LRDecayConstant, _LRStepDecay), _L1, _L2, _optimizer, _momentum, _window, _pool);
      }
    }
  }


  std::pair<Matrix, Matrix> computeLossMatrix(Matrix const& realResults, Matrix const& predicted)
  {
    if(_loss == Loss::L1)
      return L1Loss(realResults, predicted);
    else if(_loss == Loss::L2)
      return L2Loss(realResults, predicted);
    else if(_loss == Loss::BinaryCrossEntropy)
      return binaryCrossEntropyLoss(realResults, predicted);
    else //if loss == crossEntropy
      return crossEntropyLoss(realResults, predicted);
  }


  //return validation loss
  double computeLoss()
  {
    //for each layer, for each neuron, first are weights, second are bias
    std::vector<std::vector<std::pair<Matrix, std::vector<double>>>> weights(_layers.size());
    for(unsigned i = 0; i < _layers.size(); i++)
    {
      weights[i] = _layers[i]->getWeights();
    }

    //L1 and L2 regularization loss
    double L1 = 0;
    double L2 = 0;

    for(unsigned i = 0; i < weights.size(); i++)
    //for each layer
    {
      for(unsigned j = 0; j < weights[i].size(); j++)
      //for each neuron
      {
        for(unsigned k = 0; k < weights[i][j].first.size(); k++)
        //for each weight set
        {
          for(unsigned l = 0; l < weights[i][j].first[k].size(); l++)
          //for each weight
          {
            L1 += std::abs(weights[i][j].first[k][l]);
            L2 += std::pow(weights[i][j].first[k][l], 2);
          }
        }
      }
    }

    L1 *= _L1;
    L2 *= (_L2 * 0.5);

    //training loss
    Matrix input(_trainData.size());
    Matrix output(_trainRealResults.size());
    for(unsigned i = 0; i < _trainData.size(); i++)
    {
      input[i] = _trainData[i];
      output[i] = _trainRealResults[i];
    }
    input = process(input);
    double trainLoss = averageLoss(computeLossMatrix(output, input).first) + L1 + L2;

    //validation loss
    Matrix validationResult = process(_validationData);
    double validationLoss = averageLoss(computeLossMatrix(_validationRealResults, validationResult).first) + L1 + L2;

    //testing accuracy
    std::pair<double, double> testMetric;
    if(_metric == Cost::Accuracy)
    {
      testMetric = accuracy(_testRealResults, process(_testData), _classValidity);
    }
    else if(_metric == Cost::L1)
    {
      testMetric = L1Cost(_testRealResults, process(_testData));
    }
    else if(_metric == Cost::L2)
    {
      testMetric = L2Cost(_testRealResults, process(_testData));
    }


    std::cout << "   Valid_Loss: " << validationLoss << "   Train_Loss: " << trainLoss << "   First metric: " << (testMetric.first) << "   Second metric: " << (testMetric.second);
    _trainLosses.push_back(trainLoss);
    _validLosses.push_back(validationLoss);
    _testMetric.push_back(testMetric.first);
    _testSecondMetric.push_back(testMetric.second);
    return validationLoss;
  }


  void save()
  {
     for(unsigned i = 0; i < _layers.size(); i++)
      {
          _layers[i]->save();
      }
  }


  void loadSaved()
  {
     for(unsigned i = 0; i < _layers.size(); i++)
      {
          _layers[i]->loadSaved();
      }
  }


protected:
  unsigned _seed;

  std::mt19937 _generator;
  std::bernoulli_distribution _dropoutDist;
  std::bernoulli_distribution _dropconnectDist;

  std::vector<std::shared_ptr<ILayer>> _layers;

  mutable ThreadPool _pool;

  double _LRDecayConstant;
  unsigned _LRStepDecay;
  double (* _decay)(double, unsigned, double, unsigned);

  unsigned const _batchSize;
  double _learningRate;
  double _L1;
  double _L2;
  double _dropout;
  double _dropconnect;
  unsigned const _maxEpoch;
  unsigned const _patience;
  Loss _loss;

  double _validationRatio;
  double _testRatio;
  Dataset _rawData;
  Matrix _trainData;
  Matrix _trainRealResults;
  Matrix _validationData;
  Matrix _validationRealResults;
  Matrix _testData;
  Matrix _testRealResults;
  unsigned _nbBatch;

  unsigned _epoch;
  unsigned _optimalEpoch;
  std::vector<double> _trainLosses;
  std::vector<double> _validLosses;
  std::vector<double> _testMetric;
  std::vector<double> _testSecondMetric;

  double _classValidity;

  Optimizer _optimizer;
  double _momentum; //momentum
  double _window; //window effect on grads
  Cost _metric;

  std::vector<std::string> _labels;
  double _plateau;
  bool _normalizeOutputs;
  std::vector<double> _outputMeans;
};



} // namespace brain

#endif //BRAIN_NETWORK_HH_