#ifndef BURNET_LAYER_HH_
#define BURNET_LAYER_HH_

#include "Neuron.hh"
#include <iostream>
#include <thread>
#include <functional>

namespace burnet
{


class ILayer
{
public:
    virtual ~ILayer(){}
    virtual Matrix process(Matrix const& inputs) = 0;
    virtual Matrix processToLearn(Matrix const& inputs, double dropout, double dropconnect, std::bernoulli_distribution& dropoutDist, std::bernoulli_distribution& dropconnectDist, std::mt19937& dropGen) = 0;
    virtual void computeGradients(Matrix const& inputGradients) = 0;
    virtual Matrix getGradients() = 0;
    virtual unsigned size() const = 0;
    virtual void init(unsigned nbInputs, unsigned nbOutputs, unsigned batchSize) = 0;
    virtual void updateWeights(double learningRate, double L1, double L2, double momentum) = 0;
    virtual void save() = 0;
    virtual void loadSaved() = 0;
    virtual std::vector<std::pair<Matrix, std::vector<double>>> getWeights() const = 0;
};


//=============================================================================
//=============================================================================
//=============================================================================
//=== LAYER ===================================================================
//=============================================================================
//=============================================================================
//=============================================================================


template<typename Aggr_t = Dot, typename Act_t = Relu,
typename = typename std::enable_if<
std::is_base_of<Activation, Act_t>::value &&
std::is_base_of<Aggregation, Aggr_t>::value,
void>::type>
class Layer : public ILayer
{
public:
    Layer(LayerParam const& param = LayerParam(), std::vector<Neuron<Aggr_t, Act_t>> const& neurons = std::vector<Neuron<Aggr_t, Act_t>>()):
    _inputSize(0),
    _batchSize(0),
    _distrib(param.distrib),
    _distVal1(param.mean_boundary),
    _distVal2(param.deviation),
    _maxNorm(param.maxNorm),
    _k(param.k),
    _neurons(neurons.size() == 0 ? std::vector<Neuron<Aggr_t, Act_t>>(param.size) : neurons)
    {
        _localNThread = nthreads;
        _neuronPerThread = static_cast<unsigned>(size() / _localNThread);

        for(; _localNThread > 0; _localNThread--)
        {
            if(_neuronPerThread > 0)
                break;
            else
                _neuronPerThread = static_cast<unsigned>(size() / _localNThread);
        }
    }


    void init(unsigned nbInputs, unsigned nbOutputs, unsigned batchSize)
    {
        _inputSize = nbInputs;
        _batchSize = batchSize;
        for(unsigned i = 0; i < size(); i++)
        {
            _neurons[i].init(_distrib, _distVal1, _distVal2, nbInputs, nbOutputs, batchSize, _k);
        }
    }


    Matrix process(Matrix const& inputs)
    {
        //lines are features, columns are neurons
        Matrix output(inputs.size(), std::vector<double>(_neurons.size(), 0));
        std::vector<std::thread> threads;

        for(unsigned i = 0; i < _localNThread; i++)
        {
            if(i != _localNThread-1)
                threads.push_back(std::thread(performProcess, this, _neuronPerThread*i, _neuronPerThread*(i+1), std::ref(inputs), std::ref(output)));
            else
                threads.push_back(std::thread(performProcess, this, _neuronPerThread*i, size(), std::ref(inputs), std::ref(output)));
        }
        for(unsigned i = 0; i < threads.size(); i++)
        {
            threads[i].join();
        }
        return output;
    }


    Matrix processToLearn(Matrix const& inputs, double dropout, double dropconnect, std::bernoulli_distribution& dropoutDist, std::bernoulli_distribution& dropconnectDist, std::mt19937& dropGen)
    {
        //lines are features, columns are neurons
        Matrix output(_batchSize, std::vector<double>(_neurons.size(), 0));
        std::vector<std::thread> threads;

        for(unsigned i = 0; i < _localNThread; i++)
        {
            if(i != _localNThread-1)
                threads.push_back(std::thread(performProcessToLearn, this, _neuronPerThread*i, _neuronPerThread*(i+1), std::ref(inputs), std::ref(output), dropout, dropconnect, std::ref(dropoutDist), std::ref(dropconnectDist), std::ref(dropGen)));
            else
                threads.push_back(std::thread(performProcessToLearn, this, _neuronPerThread*i, size(), std::ref(inputs), std::ref(output), dropout, dropconnect, std::ref(dropoutDist), std::ref(dropconnectDist), std::ref(dropGen)));
        }
        for(unsigned i = 0; i < threads.size(); i++)
        {
            threads[i].join();
        }
        return output;
    }


    void computeGradients(Matrix const& inputGradients)
    {
        std::vector<std::thread> threads;

        for(unsigned i = 0; i < _localNThread; i++)
        {
            if(i != _localNThread-1)
                threads.push_back(std::thread(performComputeGradients, this, _neuronPerThread*i, _neuronPerThread*(i+1), std::ref(inputGradients)));
            else
                threads.push_back(std::thread(performComputeGradients, this, _neuronPerThread*i, size(), std::ref(inputGradients)));
        }
        for(unsigned i = 0; i < threads.size(); i++)
        {
            threads[i].join();
        }
    }


    void save()
    {
        for(unsigned i = 0; i < _neurons.size(); i++)
        {
            _neurons[i].save();
        }
    }


    void loadSaved()
    {
        for(unsigned i = 0; i < _neurons.size(); i++)
        {
            _neurons[i].loadSaved();
        }
    }


    //one gradient per input neuron (line) and per feature (col)
    //SHOULD THIS FUNCTION BE PARALELLIZED ?
    Matrix getGradients()
    {
        Matrix grad(_inputSize, std::vector<double>(_batchSize, 0));

        for(unsigned i = 0; i < _neurons.size(); i++)
        {
            Matrix neuronGrad = _neurons[i].getGradients();
            for(unsigned j = 0; j < neuronGrad.size(); j++)
            {
                for(unsigned k = 0; k < neuronGrad[0].size(); k++)
                grad[k][j] += neuronGrad[j][k];
            }
        }

        return grad;
    }


    void updateWeights(double learningRate, double L1, double L2, double momentum)
    {
        //check if there are more neurons than threads
        std::vector<std::thread> threads;

        for(unsigned i = 0; i < _localNThread; i++)
        {
            if(i != _localNThread-1)
                threads.push_back(std::thread(performUpdateWeights, this, _neuronPerThread*i, _neuronPerThread*(i+1), learningRate, L1, L2, momentum));
            else
                threads.push_back(std::thread(performUpdateWeights, this, _neuronPerThread*i, size(), learningRate, L1, L2, momentum));
        }
        for(unsigned i = 0; i < threads.size(); i++)
        {
            threads[i].join();
        }
    }


    unsigned size() const
    {
        return _neurons.size();
    }


    std::vector<std::pair<Matrix, std::vector<double>>> getWeights() const
    {
        std::vector<std::pair<Matrix, std::vector<double>>> weights(size());

        for(unsigned i = 0; i < size(); i++)
         {
             weights[i] = _neurons[i].getWeights();
         }

         return weights;
    }


protected:

    void performProcess(unsigned indexBeg, unsigned indexEnd, Matrix const& inputs, Matrix& output) const
    {
        for(unsigned i = indexBeg; i < indexEnd; i++)
        {
            //one result per feature (for each neuron)
            std::vector<double> result = _neurons[i].process(inputs);
            for(unsigned j = 0; j < result.size(); j++)
            {
                output[j][i] = result[j];
            }
        }
    }


    void performProcessToLearn(unsigned indexBeg, unsigned indexEnd, Matrix const& inputs, Matrix& output, double dropout, double dropconnect, std::bernoulli_distribution& dropoutDist, std::bernoulli_distribution& dropconnectDist, std::mt19937& dropGen)
    {
        for(unsigned i = indexBeg; i < indexEnd; i++)
        {
            //one result per feature (for each neuron)
            std::vector<double> result = _neurons[i].processToLearn(inputs, dropconnect, dropconnectDist, dropGen);
            for(unsigned j = 0; j < result.size(); j++)
            {
                output[j][i] = result[j];
                //dropOut
                if(dropout > std::numeric_limits<double>::epsilon())
                {
                    if(dropoutDist(dropGen))
                        output[j][i] = 0;
                    else
                        output[j][i] /= (1-dropout);
                }
            }
        }
    }


    void performComputeGradients(unsigned indexBeg, unsigned indexEnd, Matrix const& inputGradients)
    {
        for(unsigned i = indexBeg; i < indexEnd; i++)
        {
            _neurons[i].computeGradients(inputGradients[i]);
        }
    }


    void performUpdateWeights(unsigned indexBeg, unsigned indexEnd, double learningRate, double L1, double L2, double momentum)
    {
        for(unsigned i = indexBeg; i < indexEnd; i++)
        {
            _neurons[i].updateWeights(learningRate, L1, L2, _maxNorm, momentum);
        }
    }

protected:

    unsigned _inputSize;
    unsigned _batchSize;
    Distrib _distrib;
    double _distVal1; //mean (if uniform), boundary (if uniform)
    double _distVal2; //deviation (if normal) or useless (if uniform)
    double const _maxNorm;
    unsigned _k; //number of weight set for each neuron
    std::vector<Neuron<Aggr_t, Act_t>> _neurons;
    unsigned _localNThread;
    unsigned _neuronPerThread;
};



} //namespace burnet



#endif //BURNET_LAYER_HH_