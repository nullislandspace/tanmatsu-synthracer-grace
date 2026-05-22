module pillars() {
    color("#a0a0a0") {
        translate([-5.5, 0, 0]) {
            cylinder(d=1, h=10, $fn=6);
        }
        translate([5.5, 0, 0]) {
            cylinder(d=1, h=10, $fn=8);
        }
    }
}

module signbase() {
    color("#ffffff") {
        translate([-4.5, -0.125, 7]) {
            cube([9, 0.25,  3]);
        }
    }
}

module signholders() {
    color("#0c0cFF") {
        translate([-5, -0.125, 9.7]) {
            cube([0.5, 0.25, 0.3]);
        }
        translate([-5, -0.125, 7]) {
            cube([0.5, 0.25, 0.3]);
        }
        translate([4.5, -0.125, 9.7]) {
            cube([0.5, 0.25, 0.3]);
        }
        translate([4.5, -0.125, 7]) {
            cube([0.5, 0.25, 0.3]);
        } 
    }
}

module signtext() {
    color("#FF0000") {
        translate([-4, -0.3, 8]) {
            rotate([90, 0, 0]) {
                scale([0.85, 1.5, 0.25]) {
                    linear_extrude(height=0.25) {
                        text(text = "SynthEngine 3D", size=0.8, font="Liberation Mono", $fn=4);
                    }
                }
            }
        }
    }
}
    
pillars();
signbase();
signholders();
signtext();