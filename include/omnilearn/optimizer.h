// optimizer.h

#ifndef OPTIMIZER_H_
#define OPTIMIZER_H_

#include <cmath> // included here (not in .cpp) because size_t is needed



namespace omnilearn
{



void optimizedUpdate(double& coefToUpdate, double& previousGrad, double& previousGrad2, double& optimalPreviousGrad2, double& previousUpdate, double gradient, bool nesterov, bool automaticLearningRate,
                     bool adaptiveLearningRate, double learningRate, double momentum, double window, double optimizerBias, size_t iteration, double L1, double L2, double decay);



} // namespace omnilearn



#endif // OPTIMIZER_H_