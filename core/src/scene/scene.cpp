#include "scene.h"

#include "platform.h"
#include "util/stringsOp.h"

#include "directionalLight.h"
#include "pointLight.h"
#include "spotLight.h"

Scene::Scene() {

}

void Scene::addStyle(std::unique_ptr<Style> _style) {
    m_styles.push_back(std::move(_style));
}

void Scene::addLight(std::shared_ptr<Light> _light, InjectionType _type) {
    
    //  Add (inject) need code blocks (defines, structs, functions and instances) 
    //  to compute this light.
    //  
    //  NOTE:   still the MAIN "calculateLighting" function (that computes all the lights) 
    //          HAVE TO be add at the very end
    //


    //  Avoid duplications
    if (m_lights.find(_light->getName()) == m_lights.end()) {
        for (auto& style : m_styles) {
            _light->injectOnProgram(style->getShaderProgram(), _type);
        }
        m_lights[_light->getName()] = _light;
    }    
}

void Scene::buildShaders() {
    
    //  INJECT the MAIN "calculateLighting" function (that computes all the lights)
    //
    std::string vertexLightBlock = stringFromResource("lights_vert.glsl");
    std::string fragmentLightBlock = stringFromResource("lights_frag.glsl");

    if (m_lights.size() > 0) {
        std::string vertexList = "";
        std::string fragmentList = "";
        for (auto& light : m_lights) {
            if (light.second->getInjectionType() == VERTEX ||
                light.second->getInjectionType() == BOTH) {
                vertexList += light.second->getInstanceComputeBlock();
            }
            
            if (light.second->getInjectionType() == FRAGMENT ||
                light.second->getInjectionType() == BOTH) {
                fragmentList += light.second->getInstanceComputeBlock();
            }
        }

        replaceString(vertexLightBlock,"#pragma tangram: lights_to_compute",vertexList);
        replaceString(fragmentLightBlock,"#pragma tangram: lights_to_compute",fragmentList); 
    }

    for (auto& style : m_styles) {
        style->getShaderProgram()->addBlock("vert_lighting", vertexLightBlock+"\n");
        style->getShaderProgram()->addBlock("frag_lighting", fragmentLightBlock+"\n");
    }


    //  COMPILE ALL SHADERS
    //
    for (auto& style : m_styles) {
        style->getShaderProgram()->build();
    }
}
