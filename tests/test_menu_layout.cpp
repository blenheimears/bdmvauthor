// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/menu_layout.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace bdmvauthor;
static void require(bool ok, const char* message) { if(!ok) throw std::runtime_error(message); }
int main() {
    try {
        Rect aperture{480,0,2880,2160};
        Rect button{520,100,400,100}, label{1500,320,800,120}, button2{2900,700,300,140};
        std::vector<Rect*> items{&button,&label,&button2};
        require(align_menu_rects(items,MenuAlign::Left,aperture),"left alignment should move items");
        require(button.x==520 && label.x==520 && button2.x==520,"mixed buttons and labels left aligned");
        require(button.y==100 && label.y==320 && button2.y==700,"align left retains vertical positions");
        require(align_menu_rects(items,MenuAlign::VCenter,aperture),"vertical center should move items");
        require(button.y+button.height/2==label.y+label.height/2 && label.y+label.height/2==button2.y+button2.height/2,"centers align");
        button.x=520;label.x=1500;button2.x=2900;
        require(space_menu_rects(items,MenuAxis::Horizontal,48,aperture),"horizontal spacing should move items");
        require(label.x==button.x+button.width+48 && button2.x==label.x+label.width+48,"exact horizontal edge-to-edge gap");
        require(button.x>=aperture.x && button2.x+button2.width<=aperture.x+aperture.width,"spacing fits 4:3 aperture");
        const Rect before1=button,before2=label,before3=button2;
        require(!space_menu_rects(items,MenuAxis::Horizontal,1500,aperture),"oversized gap must be rejected");
        require(button.x==before1.x && label.x==before2.x && button2.x==before3.x,"rejected layout leaves all unchanged");
        button.y=100; label.y=320; button2.y=700;
        require(space_menu_rects(items,MenuAxis::Vertical,35,aperture),"vertical spacing moves items");
        require(label.y==button.y+button.height+35 && button2.y==label.y+label.height+35,"exact vertical gap");
        require(button.width==400 && label.width==800 && button2.width==300,"layout never resizes elements");
        require(!align_menu_rects({&button},MenuAlign::Right,aperture),"one element not a group");
        require(!align_menu_rects({nullptr,&button},MenuAlign::Left,aperture),"null layout target must be safely rejected");
        require(!space_menu_rects({nullptr,&button},MenuAxis::Horizontal,10,aperture),"null spacing target must be safely rejected");
        label.x=1500; button.x=520;button2.x=2900;
        require(align_menu_rects(items,MenuAlign::Right,aperture),"right align moves items");
        require(button.x+button.width==label.x+label.width && label.x+label.width==button2.x+button2.width,"right alignment accounts for different widths");
        require(align_menu_rects(items,MenuAlign::HCenter,aperture),"horizontal center moves items");
        require(button.x+button.width/2==label.x+label.width/2 && label.x+label.width/2==button2.x+button2.width/2,"centers X align");
        button.y=100;label.y=320;button2.y=700;
        require(align_menu_rects(items,MenuAlign::Bottom,aperture),"bottom align moves items");
        require(button.y+button.height==label.y+label.height && label.y+label.height==button2.y+button2.height,"bottom alignment accounts for different heights");
        std::cout<<"menu layout geometry tests passed\n";
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
