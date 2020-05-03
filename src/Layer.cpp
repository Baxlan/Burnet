// Layer.cpp

#include "omnilearn/Layer.hh"



omnilearn::Layer::Layer(LayerParam const& param):
_param(param),
_inputSize(0),
_neurons(std::vector<Neuron>(param.size, Neuron(param.aggregation, param.activation)))
{
}


void omnilearn::Layer::init(size_t nbInputs, size_t nbOutputs, std::mt19937& generator)
{
    _inputSize = nbInputs;
    for(size_t i = 0; i < _neurons.size(); i++)
    {
        _neurons[i].init(_param.distrib, _param.mean_boundary, _param.deviation, nbInputs, nbOutputs, _param.k, generator, _param.useOutput);
    }
}


void omnilearn::Layer::init(size_t nbInputs)
{
    _inputSize = nbInputs;
}


omnilearn::Matrix omnilearn::Layer::process(Matrix const& inputs, ThreadPool& t) const
{
    //lines are features, columns are neurons
    Matrix output(inputs.rows(), _neurons.size());
    std::vector<std::future<void>> tasks(_neurons.size());

    for(size_t i = 0; i < _neurons.size(); i++)
    {
        tasks[i] = t.enqueue([this, &inputs, &output, i]()->void
        {
            //one result per feature (for each neuron)
            Vector result = _neurons[i].process(inputs);
            for(eigen_size_t j = 0; j < result.size(); j++)
                output(j, i) = result[j];
        });
    }
    for(size_t i = 0; i < tasks.size(); i++)
    {
        tasks[i].get();
    }
    return output;
}


omnilearn::Vector omnilearn::Layer::processToLearn(Vector const& input, double dropout, double dropconnect, std::bernoulli_distribution& dropoutDist, std::bernoulli_distribution& dropconnectDist, std::mt19937& dropGen, ThreadPool& t)
{
    //each element is associated to a neuron
    Vector output(_neurons.size());
    std::vector<std::future<void>> tasks(_neurons.size());

    for(size_t i = 0; i < _neurons.size(); i++)
    {
        tasks[i] = t.enqueue([this, &input, &output, i, dropout, dropconnect, &dropoutDist, &dropconnectDist, &dropGen]()->void
        {
            output(i) = _neurons[i].processToLearn(input, dropconnect, dropconnectDist, dropGen);
            //dropOut
            if(dropout > std::numeric_limits<double>::epsilon())
            {
                if(dropoutDist(dropGen))
                    output[i] = 0;
                else
                    output[i] /= (1-dropout);
            }
        });
    }
    for(size_t i = 0; i < tasks.size(); i++)
    {
        tasks[i].get();
    }
    return output;
}


omnilearn::Vector omnilearn::Layer::processToGenerate(Vector const& input, ThreadPool& t)
{
    //each element is associated to a neuron
    Vector output(_neurons.size());
    std::vector<std::future<void>> tasks(_neurons.size());

    for(size_t i = 0; i < _neurons.size(); i++)
    {
        tasks[i] = t.enqueue([this, &input, &output, i]()->void
        {
            output(i) = _neurons[i].processToGenerate(input);
        });
    }
    for(size_t i = 0; i < tasks.size(); i++)
    {
        tasks[i].get();
    }
    return output;
}


void omnilearn::Layer::computeGradients(Vector const& inputGradient, ThreadPool& t)
{
    std::vector<std::future<void>> tasks(_neurons.size());

    for(size_t i = 0; i < _neurons.size(); i++)
    {
        tasks[i] = t.enqueue([this, &inputGradient, i]()->void
        {
            _neurons[i].computeGradients(inputGradient[i]);
        });
    }
    for(size_t i = 0; i < tasks.size(); i++)
    {
        tasks[i].get();
    }
}


void omnilearn::Layer::computeGradientsAccordingToInputs(Vector const& inputGradient, ThreadPool& t)
{
    std::vector<std::future<void>> tasks(_neurons.size());

    for(size_t i = 0; i < _neurons.size(); i++)
    {
        tasks[i] = t.enqueue([this, &inputGradient, i]()->void
        {
            _neurons[i].computeGradientsAccordingToInputs(inputGradient[i]);
        });
    }
    for(size_t i = 0; i < tasks.size(); i++)
    {
        tasks[i].get();
    }
}


void omnilearn::Layer::keep()
{
    for(size_t i = 0; i < _neurons.size(); i++)
    {
        _neurons[i].keep();
    }
}


void omnilearn::Layer::release()
{
    for(size_t i = 0; i < _neurons.size(); i++)
    {
        _neurons[i].release();
    }
}


//one gradient per input neuron
omnilearn::Vector omnilearn::Layer::getGradients(ThreadPool& t)
{
    Vector grad = Vector::Constant(_inputSize, 0);
    std::vector<std::future<void>> tasks(_neurons.size());

    for(size_t i = 0; i < _neurons.size(); i++)
    {
        tasks[i] = t.enqueue([this, i, &grad]()->void
        {
            Vector neuronGrad = _neurons[i].getGradients();
            for(eigen_size_t j = 0; j < neuronGrad.size(); j++)
                grad(j) += neuronGrad(j);
        });
    }
    for(size_t i = 0; i < tasks.size(); i++)
        tasks[i].get();
    return grad;
}


void omnilearn::Layer::updateWeights(double learningRate, double L1, double L2, Optimizer opti, double momentum, double window, double optimizerBias, ThreadPool& t)
{
    std::vector<std::future<void>> tasks(_neurons.size());

    for(size_t i = 0; i < _neurons.size(); i++)
    {
        tasks[i] = t.enqueue([=]()->void
        {
            _neurons[i].updateWeights(learningRate, L1, L2, _param.maxNorm, opti, momentum, window, optimizerBias);
        });
    }
    for(size_t i = 0; i < tasks.size(); i++)
    {
        tasks[i].get();
    }
}


void omnilearn::Layer::updateInput(Vector& input, double learningRate)
{
    // no parallelization here because editing the same input by multiple neurons at the same time would cause errors
    for(size_t i = 0; i < _neurons.size(); i++)
    {
        _neurons[i].updateInput(input, learningRate);
    }
}


size_t omnilearn::Layer::size() const
{
    return _neurons.size();
}


std::vector<std::pair<omnilearn::Matrix, omnilearn::Vector>> omnilearn::Layer::getWeights(ThreadPool& t) const
{
    std::vector<std::pair<Matrix, Vector>> weights(size());
    std::vector<std::future<void>> tasks(_neurons.size());

    for(size_t i = 0; i < _neurons.size(); i++)
    {
        tasks[i] = t.enqueue([this, &weights, i]()->void
        {
            weights[i] = _neurons[i].getWeights();
        });
    }
    for(size_t i = 0; i < tasks.size(); i++)
    {
        tasks[i].get();
    }
        return weights;
}


void omnilearn::Layer::resize(size_t neurons)
{
    _neurons = std::vector<Neuron>(neurons, Neuron(_param.aggregation, _param.activation));
}


size_t omnilearn::Layer::nbWeights() const
{
    return _neurons[0].nbWeights();
}


void omnilearn::to_json(json& jObj, Layer const& layer)
{
    jObj["aggregation"] = aggregationToStringMap[layer._param.aggregation];
    jObj["activation"] = activationToStringMap[layer._param.activation];
    jObj["maxnorm"] = layer._param.maxNorm;
    for(size_t i = 0; i < layer._neurons.size(); i++)
    {
        jObj["neurons"][i] = layer._neurons[i];
    }
}


void omnilearn::from_json(json const& jObj, Layer& layer)
{
    layer._param.aggregation = stringToAggregationMap[jObj.at("aggregation")];
    layer._param.activation = stringToActivationMap[jObj.at("activation")];
    layer._param.maxNorm = jObj.at("maxnorm");
    layer._neurons.resize(jObj.at("neurons").size());

    for(size_t i = 0; i < layer._neurons.size(); i++)
    {
        layer._neurons[i] = jObj.at("neurons").at(i);
        layer._neurons[i].setAggrAct(layer._param.aggregation, layer._param.activation);
    }
}