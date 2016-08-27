#include "moses/PP/TopicsPhraseProperty.h"
#include <sstream>
#include <assert.h>
#include <vector>

namespace Moses
{

void TopicsPhraseProperty::ProcessValue(const std::string &value)
{
	std::istringstream tokenizer(value);
	float temp;

	for (int t=0;t<50;t++)
	{
		tokenizer >> temp;
		Topics.push_back(temp);
		/*if (! temp) 
		{ 
			UTIL_THROW2("TopicssPhraseProperty: Not able to read topic. Flawed property?");
			assert(temp < 1  && temp > 0);
		}*/
	}
};

std::ostream& operator<<(std::ostream &out, const TopicsPhraseProperty &obj)
{
	std::vector<float> ttoopps=obj.GetTopics();
	out << "Topic property=";
	for ( int j =0; j< 50 ;j++)
	{
			out<< ttoopps[j]<<" ";
	}
	return out;
}

} // namespace Moses

