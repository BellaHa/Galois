#include <cstdio>
#include <cstdlib>

#include "Preprocessor.h"
#include "Postprocessor.h"
#include "Vertex.h"
#include "Production.h"
#include "Point2D/DoubleArgFunction.hxx"
#include "Point2D/Tier.hxx"
#include "Point2D/MatrixGenerator.hxx"

using namespace D2;

class TestFunction2D : public IDoubleArgFunction {
	double ComputeValue(double x, double y) {
		return x*x*x-y*y;
	}
};

int main(int argc, char ** argv)
{
	int nrOfTiers = 4;
	int i = 0;

	AbstractProduction *production = new AbstractProduction(5, 17, 21, 21);
	MatrixGenerator *matrixGenerator = new MatrixGenerator();
	IDoubleArgFunction *function = new TestFunction2D();
	Mes2DPreprocessor *preprocessor = new Mes2DPreprocessor();
	Postprocessor *postprocessor = new Postprocessor();

	std::list<D2::Tier*> *tiers = matrixGenerator->CreateMatrixAndRhs(nrOfTiers, 0, 0, 1, function);
	std::vector<EquationSystem*> *equations = preprocessor->preprocess(tiers);

	EquationSystem *globalSystem = new EquationSystem(matrixGenerator->GetMatrix(),
													  matrixGenerator->GetRhs(),
													  matrixGenerator->GetMatrixSize());


	globalSystem->eliminate(matrixGenerator->GetMatrixSize());
	globalSystem->backwardSubstitute(matrixGenerator->GetMatrixSize()-1);

	Vertex *S = new Vertex(NULL, NULL, NULL, ROOT, 15);

	Vertex *p1 = new Vertex(NULL, NULL, S, NODE, 15);
	Vertex *p2 = new Vertex(NULL, NULL, S, NODE, 15);

	S->setLeft(p1);
	S->setRight(p2);

	Vertex *p3 = new Vertex(NULL, NULL, p1, LEAF, 17);
	Vertex *p4 = new Vertex(NULL, NULL, p1, LEAF, 17);

	Vertex *p5 = new Vertex(NULL, NULL, p2, LEAF, 17);
	Vertex *p6 = new Vertex(NULL, NULL, p2, LEAF, 17);

	p1->setLeft(p3);
	p1->setRight(p4);

	p2->setLeft(p5);
	p2->setRight(p6);

	production->A1(p3, equations->at(0));
	production->A(p4, equations->at(1));
	production->A(p5, equations->at(2));
	production->AN(p6, equations->at(3));

	production->A2(p1);
	production->A2(p2);

	production->E(p1);
	production->E(p2);

	production->A2(S);
	production->ERoot(S);

	production->BS(S);

	production->BS(p1);
	production->BS(p2);

	production->BS(p3);
	production->BS(p4);
	production->BS(p5);
	production->BS(p6);



	std::vector<Vertex*> *data = new std::vector<Vertex*>();

	data->push_back(p3);
	data->push_back(p4);
	data->push_back(p5);
	data->push_back(p6);

	std::vector<double> *result = postprocessor->postprocess(data, equations, production);

	for (std::vector<double>::iterator it=result->begin(); it!=result->end(); ++it, ++i) {
		printf("[%4d] %.16f %.16f diff=%.16f\n", i, *it, globalSystem->rhs[i], fabs(*it-globalSystem->rhs[i]));
	}


	delete production;
	delete function;

	delete S;
	delete globalSystem;

	return 0;
}
